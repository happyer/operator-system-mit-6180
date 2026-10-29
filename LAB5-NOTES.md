# LAB 5 · cow —— 设计笔记（130/130 完成态）

> 分支 `cow` · 2026-10-22~23 · commit `d643460`
> 前置：Lab 3 的 uvmcopy/walk/页表手术经验、Lab 4 的陷入链路。本 lab 一次通关（usertests 挂起一次，当场定位修复）。

---

## 1. 任务构成（总分 130）

| 任务 | 分值 | 考什么 |
|---|---|---|
| cowtest `simple` | 30 | sbrk 2/3 物理内存再 fork——无 COW 必失败；连跑两遍抓内存泄漏 |
| cowtest `three` | 30 | 三进程都写同一批 COW 页；验证复制后的页被正确释放 |
| cowtest `file` | 20 | 管道 read 进 COW 页——专考 **copyout** 路径 |
| cowtest `forkfork` | 20 | 100 轮嵌套 fork——专考 **refcnt 竞态** |
| usertests copyin/copyout | 5+5 | 内核拷贝路径 |
| usertests 全量 | 19 | 回归（stacktest 曾在此挂起，见 §5） |
| time.txt | 1 | |

## 2. 通俗故事（三条规矩）

老规矩：fork = 整柜文件全复印，孩子十有八九转身 exec 全扔掉——白复印。
新规矩 COW：
1. **生娃不复印，父子共用原件**，但每份贴封条（清 W）盖章（PTE_COW）；
2. **谁写字硬件就报警**（store page fault），内核给报警的人单独复印一张、撕封条（恢复 W）；
3. **纸只有没人用了才扔**——每页配计数器，fork 共用 +1，谁放手 -1，减到 0 才回废纸篓。

对应三处代码：**fork 的路（uvmcopy）、报警的路（vmfault/cowfault/copyout）、扔东西的路（kfree）**。

## 3. 两件工具 + 三条路（实现细节）

### 工具一：封条 `#define PTE_COW (1L << 8)`（riscv.h）
- RSW 位（bit 8-9）硬件无视、软件专用；PTE_FLAGS 掩码 0x3FF 自然携带它。
- 存在意义：区分"**原本可写、被 fork 降权**"（写→复印）与"**天生只读 text**"（写→杀）。两种页都是只读，处理相反。

### 工具二：账本 `uchar refcnt[(PHYSTOP-KERNBASE)/PGSIZE]`（挂在 kmem 结构里）
- 计数是**物理页**的属性（不是映射的），所以单独记账；索引 = (pa-KERNBASE)/4096。
- uchar 够：一页最多被 NPROC=64 个进程共用。
- 复用 kmem.lock（计数增减总与分配/释放纠缠）。
- 新增 `cow_lock`（kalloc.c 定义、kinit 初始化、vm.c extern）：**锁顺序 cow_lock → kmem.lock，永不反向**。

### 路 1 · uvmcopy：复印改共用
```c
if((flags & PTE_W) && (flags & PTE_U)){      // ★ 必须带 PTE_U! 见 §5 guard page 地雷
  *pte = (*pte & ~PTE_W) | PTE_COW;          // 父亲: 撕W盖章
  flags = (flags & ~PTE_W) | PTE_COW;        // 孩子: 同样
}
ref_incr((void*)pa);                          // ★ text 页也要 +1! (孩子 exit 会 kfree 它)
if(mappages(new, i, PGSIZE, pa, flags) != 0){
  kfree((void*)pa);                           // 回滚 +1 (kfree 就是减账入口)
  goto err;
}
```
- 父亲 PTE 也要改：封条贴两边，否则父亲一写就污染孩子。
- fork 失败父亲停留在降权态不回滚：之后写它触发 cowfault 自我复制，语义仍正确。

### 路 2 · 报警路：cowfault 车间 + vmfault 分支 + copyout 补检
```c
uint64 cowfault(pagetable_t pt, uint64 va){   // 返回新 pa; 0 = 失败(杀)
  acquire(&cow_lock);
  pte = walk(pt, va, 0);
  if(pte==0 || !(*pte&PTE_V)){ release; return 0; }
  if(!(*pte & PTE_COW)){                       // ★ double-check: 别的 CPU 已复制完
    pa = PTE2PA(*pte); release; return pa;
  }
  pa_new = kalloc();  if(!pa_new){ release; return 0; }   // 没纸 → 杀
  memmove(pa_new, pa_old, PGSIZE);
  kfree(pa_old);                                 // 旧页减账 (到0才真回收)
  *pte = PA2PTE(pa_new) | ((PTE_FLAGS(*pte) | PTE_W) & ~PTE_COW);  // 换纸+恢复W+撕章
  release(&cow_lock);
  return pa_new;
}
```
- **vmfault 里 COW 分支必须在 `ismapped` 之前**（COW 页是 mapped 的，ismapped 会说"没事"然后写指令重试→死循环）。
- **vmfault 返回的是 pa 不是 bool**（copyout 拿返回值当 pa0 用）——2025 版约定。
- **read fault 落在 COW 页 = 异常**（R 一直在，正常读不 fault）→ return 0 杀。返回 1 会造成无限 fault 循环（§5 实战踩中）。
- **copyout 必须手动查 COW**：内核按物理地址直写用户页，CPU 不替它报警；file 测试的管道 read 就走这条路。改后 pa0 必须更新为 cowfault 返回的新页。
- copyin（内核读用户页）**不用改**：读共享原件语义天然正确。

### 路 3 · kfree：单点收账
```c
acquire(&kmem.lock);
if(refcnt[idx] > 0) refcnt[idx]--;     // ★ >0 才减: kinit 的 freerange 对 refcnt=0
if(refcnt[idx] > 0){ release; return; } //   的页调 kfree, 无脑减会 uchar 下溢回绕 255
memset(pa, 1, PGSIZE);                  // ★ junk 填充挪到"确认真回收"之后
r->next = kmem.freelist; kmem.freelist = r;
release(&kmem.lock);
```
- **为什么藏进 kfree 而不是各出口自己减**：exit/exec/回滚/uvmfree 等七八个出口殊途同归都调 kfree——单点收账永不遗漏；而"加账"只有 fork 一个场景，主动上报（ref_incr）。
- kalloc 配套：弹出页时 `refcnt[idx] = 1`。

### 改动清单
riscv.h(1 宏) · kalloc.c(refcnt+cow_lock+kinit+kfree+kalloc+ref_incr) · vm.c(前置声明+cowfault+vmfault 分支+uvmcopy+copyout) · defs.h(ref_incr) · **trap.c 零改动**（vmfault 调用点现成）。

## 4. fork 全链路复习成果（本会话问答沉淀）

- **"fork 返回两次"是错觉**：真相 = 两份 trapframe 的 epc 指向同一条用户指令（ecall 下一条，usertrap 已 +4），两个进程各自 sret 落地一次，a0 不同。
- **返回值双通道**：父亲的 pid 走"C 返回值接力"（np->pid → kfork return → sys_fork → syscall.c:141 写父 trapframe->a0 → sret）；孩子的 0 走"档案手改"（kfork 里 `np->trapframe->a0 = 0`，复印后、syscall 写父档案前）。两通道解耦：调度时机不影响任何返回值。
- **pid = allocpid()**：全局 nextpid 自增（pid_lock 防多 CPU 重号），单调不复用；登记在 fork 瞬间完成，与孩子何时运行无关。
- **RUNNABLE = 挂号，不是入队**：xv6 无就绪队列，proc[64] 数组 + state 字段就是队列；scheduler 死循环扫描"拉取"。kfork 里 `np->state = RUNNABLE` 就是"进入调度"的全部手续。
- **swtch 换魂**（demo 实锤）：13 sd 存旧世界（ra/sp/s0-s11）→ 13 ld 装新世界 → `ret` 发射（pc←ra）。孩子第一次被切入时 ra=forkret（allocproc 出厂预埋 proc.c:146-147）、sp=内核栈顶。
- **forkret 是无弦之箭**：全 xv6 无人 call 它；唯一入口是 swtch 的 ret。`ret = jalr x0,0(ra)` 不写 ra → 进 forkret 时 ra 是残留值——但 forkret 永不 ret（结尾函数指针调用 trampoline_userret 单程飞走，userret csrw satp + sret 后内核栈不可见）。forkret 一生只走一次；正常切回走 sched() 中间续播。backtrace 在 forkret 里只能爬一帧（[fp-16]=context.s0=0）——"孩子没有过去"。
- **prepare_return 的完整职责**（2025 版）：w_stvec(uservec) + 填 trapframe kernel_* 四项 + sstatus.SPP←User + **w_sepc(trapframe->epc)**。sret 双料：pc←sepc（落哪）+ 特权级←SPP（以什么身份落）。
- **ra 三个时态**：活跃在寄存器（x1）；嵌套时逐层存栈（[fp-8]，backtrace 爬的链）；被切走时冻进 context.ra（swtch）。

## 5. guard page 地雷排障记（本 lab 最精彩一战）

**现象**：cowtest 四连全绿，grade 的 usertests 却挂死 15+ 分钟。
**定位**：手动跑 usertests → tail 停在 `stacktest`（前面全 OK）。
**读题**：stacktest 让孩子读 `r_sp() - USERSTACK*PGSIZE`（栈 guard page 区域），期望被杀（xstatus==-1）。
**根因链**：
1. guard page = `V|R|W 无U`（exec 的 uvmclear 只清 U，**W 还在**——Lab 3 发现的 perm 0x07 页）
2. uvmcopy 只判 `PTE_W` → **guard page 被误盖 COW 章、撕了 W**
3. 孩子读它：无 U → load fault → 进 COW 分支 → `if(read) return 1` "假装处理成功"
4. 页权限没变 → 回去再读 → 再 fault → return 1 → **无限循环**
**修复两处**：
- uvmcopy 盖章条件加 `&& (flags & PTE_U)`：guard page 是"永久禁入页"不是"待复印共享页"
- COW 分支 read fault 改 `return 0`：COW 页上 read fault 本身即异常（R 恒在），该杀
**验证**：stacktest 孩子应声被杀（`unexpected scause 0xd`），ALL TESTS PASSED。
**教训**："无 U 但有 W"的页不是普通可写页；**任何按 W 位分类的逻辑都要先问"这页给谁用"**。Lab 3 的悬案（0x07 之谜）在 Lab 5 变成真地雷——知识是咬合的。

## 6. 方法论：三 demo 实锤（本会话新增）

| demo | 实锤什么 |
|---|---|
| /tmp/trapdemo.c | 从结构体偏移取函数指针间接调用 + 返回值在调用点下一句被接住（≈uservec/usertrap/userret） |
| /tmp/trapdemo2.c + uservec_demo.s | **汇编 label 版**：blr 的下一条放全局 label，C return 滑入；nm 证 label = blr+4（≈userret = jalr+2，kernel.asm 实证 0x8000609a→0x8000609c） |
| /tmp/swtch_demo.c + .s | **swtch 换魂**：预置 ctxB.ra=forkret_demo，swtch 的 ret 直接跳过去，main 的下一行永不执行 |

套路：**源码说意图，.asm 说事实，demo 证行为**——三者互证才算懂。
（另：tick=0.1s 三重验证——trap.c 注释 + 10MHz 算术 + qemu DTB `timebase-frequency=<0x989680>`；Mac 本机 24MHz 双路验证 mach_timebase_info + cntfrq_el0。）

## 7. 坑速查表

| # | 坑 | 拆法 |
|---|---|---|
| 1 | guard page（V\|R\|W 无U）被当 COW 页 | 盖章条件带 PTE_U |
| 2 | COW 页 read fault 返回"成功" | return 0 杀（read fault 于 COW 页必异常） |
| 3 | text 页忘 ref_incr | 孩子 exit 会 kfree 父亲在用的代码页 |
| 4 | kfree 的 memset junk 在判定前 | 挪到"确认真回收"后，否则涂花在用页 |
| 5 | freerange 开机对 refcnt=0 页 kfree | `>0` 才减，防 uchar 下溢 255 |
| 6 | 双 CPU 同时 fault 同一 COW 页 | cow_lock + 锁内 double-check |
| 7 | cowfault 返回 bool 而非 pa | copyout 拿返回值当 pa0；必须返回新物理地址 |
| 8 | COW 分支放 ismapped 之后 | COW 页 IS mapped，会被"没事"挡掉 → 写重试死循环 |
| 9 | cowfault 定义在 copyout 之后 | vm.c 头部加前置声明（-Werror=implicit-declaration） |
| 10 | multi_replace 空行不匹配（again） | od -c 会看花眼，**cat -et 才是王道** |

## 8. 成果

```
d643460 cow lab: copy-on-write fork -> 130/130
```

五连冠：util 131 → syscall 45 → pgtbl 41 → traps 95 → **cow 130**（累计 442 分）。
下一个：Lab 6 `thread`（uthread 用户线程 = 手搓 swtch 换魂术的用户态版；philosopher 用餐）——今天吃透的 swtch/forkret 直接就是它的教材。
