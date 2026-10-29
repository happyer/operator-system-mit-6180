# LAB 4 · traps —— 设计笔记（95/95 完成态）

> 分支 `traps` · 2026-10-21~22 · commits：
> `c51a2b7`(backtrace) → `4d62080`(answers) → `c417bc3`(alarm, 95/95)
> 前置：Lab 2 吃透了陷入链路（uservec/usertrap/syscall 分派），Lab 3 吃透了页表与 trapframe 页的映射。

---

## 1. 任务构成（总分 95，不是 105）

| 任务 | 分值 | 内容 |
|---|---|---|
| `answers-traps.txt` | 5 | RISC-V 汇编 5 问（基于 user/call.asm） |
| `backtrace` | 10 | 内核栈帧链回溯，挂 `sys_pause` + `panic` |
| `alarm` test0-3 | 20+20+10+10 | sigalarm/sigreturn 用户级"信号" |
| `usertests` | 19 | 全量回归（57.3s 零失败） |
| `time.txt` | 1 | |

---

## 2. RISC-V 汇编速成（call.asm 逐行读出的结论）

### 2.1 每个函数的三幕结构（g/f/main/start/strcpy… 全同款）

```
开场白: addi sp,sp,-16   开栈帧(栈向低地址生长)
        sd   ra,8(sp)    存返回地址     → 即 [fp-8]
        sd   s0,0(sp)    存调用者的帧指针 → 即 [fp-16]
        addi s0,sp,16    立新帧指针 s0 = 进函数前的旧 sp = 本帧最高地址
正题:   ...(长短随函数)
谢幕:   ld ra,8(sp) / ld s0,0(sp) / addi sp,sp,16 / ret(=jalr x0,0(ra))
```

### 2.2 s0 的一借一还（本 lab 最大的概念澄清）

- s0 是 **callee-saved**（传家宝）寄存器：被调用者可用，但返回前必须恢复原值。
- `sd s0,0(sp)`（寄存）→ `addi s0,sp,16`（g 在任期间用作帧锚）→ `ld s0,0(sp)`（物归原主）。
- 简单函数（g/f/main）里 s0 "立而不用"——不白立：①维持 backtrace 链 ②ABI 统一纪律。
- 复杂函数里 s0 真忙：`gets` 的局部变量在 `-97(s0)`；`fprintf` 用 s0 当 **varargs 卸货区**基址（`sd a2,0(s0)` … `sd a7,40(s0)`，`va_list ap = s0`）——这就是 `printf("x=%d y=%d",3)` 的 y 打出垃圾值的物理基础（a2 槽里的残留值）。

### 2.3 其它实测结论（answers 五题素材）

- 参数寄存器 a0-a7；`13` 在 **a2**（a0=格式串、a1=12、a2=13）。
- **f 和 g 都被内联进 main**，`f(8)+1` 被常量折叠成 `li a1,12`——main 里没有任何 `jal f/g`。
- printf 在 **0x724**（`jal 724 <printf>`）；jal/jalr 后 **ra = 0x3c**（调用指令的下一条）。
- `addiw` 的 w = 只对低 32 位运算再符号扩展（C 的 int 是 32 位）——看汇编认 C 类型的信号弹。
- 机器码有 2 字节（压缩指令 C 扩展，如 `1141`）和 4 字节两种，语义等价。
- 叶子函数也建帧：`-O2` 会省略帧指针（Linux 内核因此需要 ORC unwinder），xv6 用 `-Og -g` 全员保留——backtrace 20 行就能写完的前提。

### 2.4 编译期地址 vs 运行期翻译（satp 之问）

- `user.ld` 的 `. = 0x0`：所有用户程序都链接在虚拟地址 0，**编译期只有图纸（va），没有地块（pa）**。
- exec 兑现图纸：uvmalloc 建映射、loadseg 搬内容（Lab 3 print_pgtbl 实测：va 0 → pa 0x8577E000）。
- 运行期每条访存指令经 MMU+satp 自动翻译，程序无感知。
- **satp 只有内核碰**（S 态特权指令）：prepare_return 存内核 satp、usertrap 返回用户 satp、forkret 首飞。全 xv6 仅 4 处。
- 隔离靠页表不靠链接地址：两个进程同跑 call，va 0x724 翻译到各自物理页。

---

## 3. backtrace：栈里藏着一条隐式单向链表

### 3.1 原理

每个栈帧 = 链表节点 `{ 货物: [fp-8]=本层ra, next: [fp-16]=上层fp }`（与 kalloc 的 `struct run` 同构，只是隐式）。回溯 = 从 `r_fp()`（内联汇编读 s0）出发沿 next 爬链。

```c
uint64 fp = r_fp();
uint64 page = PGROUNDDOWN(fp);
printf("backtrace:\n");
while(fp != 0 && PGROUNDDOWN(fp) == page){
  printf("%p\n", (void*)*(uint64 *)(fp - 8));   // %p 必须 (void*) 强转(-Werror=format)
  fp = *(uint64 *)(fp - 16);
}
```

### 3.2 终止条件两个坑

- **❌ `fp < PGROUNDUP(kstack)` 会炸内核**：链尾 usertrap 帧的 [fp-16] 是**用户程序的 s0**（uservec 存了用户 s0 进 trapframe 但不改 s0 寄存器，usertrap 开场白把它当"上层 fp"存了）——用户地址 < 栈顶条件仍真，解引用未映射地址 → kerneltrap → panic。
- **✅ PGROUNDDOWN 判等**：用户 s0 必然不在内核栈页，一查即停。用户 s0 的"漏入"是天然哨兵。

### 3.3 第 4 行之谜（为什么恰好打印 3 个地址）

帧链有 4 帧（backtrace←sys_pause←syscall←usertrap），但只打印 3 行：
**usertrap 是栈上第一个帧**，sp 从栈顶（kstack+4096）起步，故它的 fp = sp+帧大小 恰好落在**页边界**。`PGROUNDDOWN(页边界) = 下一页 ≠ 栈页` → 循环在打印 trampoline 地址之前终止。最外层帧的 fp 恒等于栈顶页界，判界天然停在"次外层"。

### 3.4 验证

bttest 输出 3 地址 → `riscv64-unknown-elf-addr2line -e kernel/kernel`：
`0x80001e18→sysproc.c:72`（sys_pause 调 backtrace 处）、`0x80001d04→syscall.c:141`（`syscalls[num]()` 处）、`0x80001a90→trap.c:80`（usertrap 调 syscall 处）。
grade 正则 `^0x000000008` 恰好匹配 3 行（trampoline 地址是 0x0000003f… 开头，不被计数）。

### 3.5 挂点

`sys_pause` 开头（bttest 触发）+ `panic`（`printf("%s\n",s)` 之后）。panic 路径安全：`panicking=1` 使 printf 跳过锁，不死锁。

---

## 4. alarm：劫持一次普通时钟中断的"返程票"

### 4.1 语义（一句话本质）

> 用户把"定时打断我"的权力委托给内核：每消耗 n 个 **CPU tick**，不管正在执行哪条指令，强行拽去执行 handler；handler 调 sigreturn() 后原地满血复活（32 寄存器 + epc 分毫不差），用户程序全程无感。

真实世界原型 = UNIX 信号 SIGALRM。handler 不需要伪造返回地址：协议钦定它必须以 sigreturn 结尾（普通 ecall），复活由内核完成——**协议设计消解了机制难题**。

### 4.2 关键认知四连

1. **不是"中断里再中断"**：中断只发生一次且正常结束；内核只是在这次时钟中断的返程票（trapframe->epc）上改了目的地。handler 跑在 **sret 之后的用户态正常时段**，不在内核、不在中断上下文。
2. **tick 语义 = 进程实际消耗 CPU 的时间**：时钟中断全局发生，只有"正在 hart 上跑用户代码的进程"才进 usertrap 计数。sleeping/RUNNABLE 排队的进程 ticks 不涨（睡 50 tick 醒来不会立刻触发）。计数放 usertrap 而非全局 timer handler 正是此语义的自然实现。
3. **劫持 epc ≠ 立即执行**：触发时刻只改 trapframe 里的数；真正落地在下次被调度 → userret → sret。trapframe 是 per-process 档案，插队多久都不怕。
4. **handler 期间 tick 照响**：handler 也是用户代码，每 tick 同样进 usertrap 转一圈，被 `handling=1` 挡住不再改票（test2 考点）；sigreturn 时清标志 + 清计数（re-arm），两者必须同时做，否则复活瞬间机关枪式重触发。

### 4.3 双档案：为什么必须新开 alarm_tf 而不复用 trapframe

**trapframe 是活工作台**——每次陷入 uservec 都把当时的 32 寄存器整页倒进去覆盖。handler 第一行 printf 就是一次 ecall，**被打断循环的现场会被 handler 的寄存器冲掉**。alarm 场景里 trapframe 要装两代人的现场，所以篡改 epc 之前必须把第一代抄进 `alarm_tf`（寄存柜）。

- 顺序铁律：**先备份，后篡改**。反过来则备份到已篡改的 epc=handler，sigreturn 还原后无限循环。
- alarm_tf **内嵌 struct proc**（320B）而非独立页：只有内核 C 读写（不需要汇编按写死地址访问），零 kalloc 零生命周期管理。对比：trapframe 页必须独立+映射进用户页表（uservec/userret 汇编按 TRAPFRAME 固定地址存取）；Lab 3 的 usyscall 页必须映射（用户态读）。
- 备份里的 kernel_satp/sp/trap 三字段无需担心——还原后 prepare_return 总会重填。

### 4.4 四个致命陷阱（全部预判成功 → test0-3 一次全过）

| # | 陷阱 | 解法 |
|---|---|---|
| 1 | **a0 污染**：sys_sigreturn 返回后 `syscall()` 执行 `trapframe->a0 = 返回值`，把刚还原的 a0 冲成 0 | `return p->alarm_tf.a0;`——让覆盖恰好写回正确值（以毒攻毒） |
| 2 | **handler=0 合法**：periodic 就链接在地址 0，用 handler!=0 判断"设没设闹钟"永远不触发 | 判据用 `alarm_interval != 0`（sigalarm(0,0) 才是解除） |
| 3 | **epc ±4**：usertrap 只对 syscall 分支 epc+=4（sepc 指向 ecall 本身）；时钟中断的 sepc = 被打断指令 | 备份/还原零修正，直接倒回 |
| 4 | **进程槽复用**：freeproc/allocproc 不清 alarm 字段 → 新进程带上任闹钟 | 两处显式清零 4 字段 |

### 4.5 完整时间线（test0 实测路径）

```
[U] while 循环
──tick── [S] uservec(存32寄存器) → usertrap: 计数到点 → alarm_tf=备份 + handling=1 + epc=handler → yield
[S] 调度 → userret(倒回寄存器, sepc←epc) → sret
[U] ★periodic() 执行（普通用户函数，被 sret 空投而来）
     ├ printf("alarm!") ─ [S] ecall→sys_write →[U] 回来
     └ sigreturn()      ─ [S] ecall→sys_sigreturn: *trapframe=alarm_tf + handling=0 + ticks=0
[S] userret: sret 按还原的 epc 落地
[U] while 循环从被打断指令继续，寄存器分毫未变 ← 复活
```

与 fork 魔术对照：fork = 拷贝 trapframe + 单向改 a0/epc（孩子醒在 ecall 下一条，返回 0）；alarm = 备份 + 改 epc + 日后整体还原。**两次 sret 空投**（先进 handler，再回中断点），每次醒来在哪完全由 sret 时刻 trapframe->epc 决定。

### 4.6 改动清单（9 处）

proc.h（4 字段+alarm_tf）· syscall.h（22/23）· syscall.c（extern+表）· sysproc.c（sys_sigalarm/sys_sigreturn）· trap.c（which_dev==2 分支）· proc.c（allocproc/freeproc 清零）· usys.pl · user.h（#ifdef LAB_TRAPS 声明）· Makefile（_alarmtest）

---

## 5. uservec / userret：陷入旅程的两个大厅 + "滑行道"

### 5.1 为什么必须是汇编

陷入瞬间内核一无所有：sp 是用户的、寄存器全是用户值、页表是用户的。唯一可依赖 = 写死地址的 TRAPFRAME 页。uservec 用它搭好舞台（存 32 寄存器 → 换内核栈/tp/页表）才能 `jalr` 进 C；返程 sret 也只能汇编干。

### 5.2 uservec 六步（入境大厅）

① `csrrw sp,sscratch,sp` 凭空拿到 sp=TRAPFRAME ② sd×32 存用户寄存器（**覆盖现场就在这步**）③ sp←kernel_sp ④ tp←kernel_hartid、t0←kernel_trap ⑤ csrw satp←kernel_satp + sfence ⑥ `jalr t0` 进 usertrap

### 5.3 userret 五步（出境大厅）

① `csrw satp,a0` 切用户页表 ② sp←TRAPFRAME ③ 重填 kernel_* 四项（为下次陷入备料）④ ld×32 倒回寄存器（sepc←epc，**alarm 篡改在此生效**）⑤ sret

### 5.4 滑行道：usertrap 怎么"跳"到 userret —— 根本不是跳

```asm
8000609a:  jalr t0          ; ra ← 0x8000609c (下一条指令)
8000609c: <userret>:        ; ★ 符号表实证: userret = jalr + 2 (压缩指令)
8000609c:  sfence.vma
800060a0:  csrw satp, a0    ; 接住 usertrap 的返回值
```

2025 版 `usertrap` 签名改为**返回 satp**（`return satp;` 编译为 `mv a0,s1` + ret），一次普通 C 返回自然滑入紧邻的 userret，返回值经 a0 递到手上。forkret 走同一条滑行道（proc.c:532-534）。返回前 `prepare_return()` 布景：stvec←uservec + 重填 kernel_* 四项。

trampoline 页的存在意义：csrw satp 的下一条指令必须在新旧两张页表同址同页（MAXVA-PGSIZE，同一物理页映射在两张页表顶端），换世界时脚下大地不动。

### 5.5 双 demo 实锤（方法论：源码说意图，.asm 说事实，demo 证行为）

- 纯 C 版（/tmp/trapdemo.c）：offsetof(kernel_trap)=16 ✓、从结构体偏移取函数指针间接调用 ✓、返回值在调用点下一句被接住 ✓。
- 汇编 label 版（/tmp/trapdemo2.c + uservec_demo.s，macOS ARM64）：`.s` 里 `blr x8`（≈jalr t0）的下一条放全局 label `_slid_in_label`，C 函数 return 后滑入 label 执行（landed 打印收到的 a0）；符号表实证 label 地址 = blr + 4，与 xv6 的 userret = jalr + 2 完全同构。
- 本项目两次"反汇编裁决源码疑问"：Lab 2 cat.asm 找 `jal 3d8 <read>`；Lab 4 kernel.asm 证滑行道。

---

## 6. 坑速查表

| 坑 | 教训 |
|---|---|
| `fp < PGROUNDUP(kstack)` 判界 | 链尾漏入用户 s0 后条件仍真 → 解引用未映射地址炸内核；必须 PGROUNDDOWN 判等 |
| handler 地址 0 | 0 是合法地址不是 null 判据（与 Lab 2 "用户指针可为 0" 同款） |
| sigreturn 返回 0 | syscall() 的 a0 覆盖会污染还原现场；返回备份 a0 以毒攻毒 |
| 先篡改后备份 | 备份到已改的 epc → 无限循环；顺序铁律 |
| re-arm 忘清 ticks | handler 返回瞬间立即重触发（机关枪） |
| `%p` 喂 uint64 | -Werror=format 拦截，(void*) 强转（Lab 3 同款） |
| sed `/start/,/$/p` | `/$` 匹配一切行 → 只输出 2 行；用 `grep -A N` |
| Makefile 续行缩进 | 是 tab 不是空格，replace 前先 od -c 看字节 |

---

## 7. 成果

```
c417bc3 traps lab: sigalarm/sigreturn -> 95/95   (alarm 60 + usertests 19 + time 1)
4d62080 traps lab: answers-traps.txt             (5)
c51a2b7 traps lab: backtrace                     (10)
```

四连冠：util 131/131 → syscall 45/45 → pgtbl 41/41 → **traps 95/95**。
下一个：Lab 5 `cow`（写时复制）——正好接住 superpg 里"uvmcopy 无脑深拷贝"的伏笔。
