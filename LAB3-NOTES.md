# Lab 3 (`pgtbl`) 设计与调试笔记

> 分支 `pgtbl`，commits `aa2d0d0` / `ec83be7` / `1d19821` / `4cf6a60`
> `./grade-lab-pgtbl` = **41/41** ✅（ugetpid 10 + print_kpgtbl 10 + superpg 15 + answers 5 + time 1）
> ⚠️ `time.txt` 当前是占位值 `30`（沿用 Lab 2），需按实际小时数改

---

## 0. 2025 版 Lab 3 真实构成（grade 脚本 × 讲义双重验证）

| 任务 | 分值 | 一句话 |
|---|---|---|
| `ugetpid`（Speed up syscalls） | 10 | 每进程一页只读共享页 USYSCALL，用户态免陷入读 pid |
| `print_kpgtbl`（vmprint） | 10 | 递归打印整棵页表树，格式被正则逐行卡死 |
| `superpg`（2MB 超级页） | 15 | L1 叶子管 2MB；含 fork 克隆、部分释放降级 |
| `answers-pgtbl.txt` | 5 | 三道问答（PTE 解读 / 还能加速谁 / vmprint 对照） |
| `usertests` | 0（守门） | 55.8s 通过，零回归 |

旧版（2023/24）的 `sys_pgaccess` 已被 `superpg` 取代。框架预埋：`sys_pgpte`/`sys_kpgtbl`
系统调用已给、用户态 `ugetpid`/`pgpte` 已给、`walk()` 里 `#ifdef LAB_PGTBL` 的
`PTE_LEAF` 提前返回分支已给。**我们只改内核：proc.h / proc.c / vm.c / kalloc.c / defs.h。**

---

## 1. ugetpid：内核↔用户共享只读页（Linux vDSO 同款思想）

**动机**：`getpid()` 是完整 ecall 往返（几百条指令 + 两次特权级/页表切换），只为读一个
永不改变的 int。方案：创建进程时把 pid 抄到用户态可读的只读页，之后 `ugetpid()` 是
一条普通 load。

**布局**（memlayout.h）：`USYSCALL = TRAPFRAME - PGSIZE`，`struct usyscall { int pid; }`。
用户地址空间顶部三明治：TRAMPOLINE(R+X 无U) / TRAPFRAME(R+W 无U) / **USYSCALL(R+U 无W)**。
TRAPFRAME 无 U 是因为碰它的 uservec/userret 在 S 态；USYSCALL 给 U 态读所以必须有 U、
绝不能有 W（否则可伪造 pid）。

**5 处改动**（全部照抄 trapframe 生命周期模板）：

| # | 位置 | 改动 |
|---|---|---|
| 1 | `proc.h` struct proc | `struct usyscall *usyscall;` |
| 2 | `allocproc()` | kalloc + `p->usyscall->pid = p->pid`（**必须在 proc_pagetable 之前**，映射要用其物理地址） |
| 3 | `freeproc()` | kfree + 置 0（回滚安全） |
| 4 | `proc_pagetable()` | `mappages(USYSCALL, PGSIZE, pa, PTE_R\|PTE_U)`，失败回滚拆前面 |
| 5 | `proc_freepagetable()` | `uvmunmap(USYSCALL, 1, 0)`（在 uvmfree 拆树前） |

**fork/exec/userinit 零改动**：2025 版 userinit 直接调 allocproc；kfork 的
`uvmcopy(0..sz)` 够不到天花板页；孩子出生自带自己的 pid。
**绝不能**在 kfork 里加 `*(np->usyscall) = *(p->usyscall)` —— 串号，ugetpid_test
的 64 代 fork 验证当场抓住。

**彩蛋**：print_pgtbl 先于 ugetpid_test 运行时 USYSCALL 的 pte=0x13（无 A 位）；
跑完 ugetpid_test 后变 0x53 —— 硬件自动盖 Accessed 章。

---

## 2. vmprint：递归打印页表树

**核心**：`vmprint_walk(pagetable, level, va_base)` 先序 DFS。

- **va 重建**（唯一难点）：PTE 不存 va，递归时带 `va_base`，
  `va = va_base | (i << PXSHIFT(level))` —— PX 切片的逆运算。
- **缩进**：深度 = `3 - level`，每层打一个 `" .."`（空格+两点，grade 正则硬匹配）。
- **格式**：`printf("%p: pte %p pa %p\n", ...)`；`%p` = printptr = `0x`+16位hex。
  内核 printf 声明带 format 属性 → **实参必须 `(void*)` 强转**否则 -Werror 拦截。
- **PTE_LEAF 守卫**：叶子只打不递归（为任务 3 的 L1 叶子提前埋好）。
- 只打 V=1；中间节点（R|W|X 全 0）先打自己再递归子树。

调用链：`pgtbltest kpgtbl() → sys_kpgtbl → vmprint(p->pagetable)`（打的是**当前
进程的用户页表**，名字唬人）。

**实测输出解读**（answers Q1/Q3 素材）：

| va | perm | 身份 |
|---|---|---|
| 0x0/0x1000 | 0x5B V R X U A | text 段（2025 版用户程序链接在 0） |
| 0x2000 | 0x17 V R W U | data 段 |
| **0x3000** | **0x07 V R W 无U** | **栈 guard page**！exec.c 分配 USERSTACK+1 页后 `uvmclear` 清低页 U 位，用户态越界触栈 → fault → 杀 |
| 0x4000 | 0xD7 V R W U A D | 栈页（exec 写入 argv → A+D） |
| 0x3FFFFFD000 | 0x13 V R U | USYSCALL（本 lab 新增） |
| 0x3FFFFFE000 | 0xC7 V R W A D | TRAPFRAME（每次 ecall 都写） |
| 0x3FFFFFF000 | 0x4B V R X A | TRAMPOLINE，pa 在内核 text，全进程共享同一物理页 |

---

## 3. superpg：2MB 超级页（本 lab 的主菜）

### 3.1 硬件语义

叶子在哪层就管多大地盘：L0 叶子=4KB（va 低 12 位是偏移）；**L1 叶子=2MB（va 低
21 位是偏移，pa 必须 2MB 对齐**，否则 misaligned superpage fault）。
`walk()` 的 LAB_PGTBL 分支：下钻途中 L1 撞叶子（V=1 且 R|W|X 非 0）提前返回。

### 3.2 四步实现

**① kalloc.c 圈地**：`kinit` 里 `freerange(end, PHYSTOP - NSUPER*SUPERPGSIZE)`，
内存顶端 NSUPER=20 块（40MB）不进 4KB 链表。`supermem.used[20]` 账本 + 自旋锁。
`superalloc()`（领块+memset 2MB）/ `superfree()`（归还，校验对齐+在用）/
`is_super(pa)`（pa ∈ 预留区）。**kfree 加防御：is_super(pa) → panic**。
峰值账：superpg_fork 父 8 块 + 子 8 块 = 16 ≤ 20 ✓。

**② vm.c 三件**：
- `walkl1()`：walk 的孪生，只下钻一层（L2→L1），返回 L1 格子地址。
- `mappages` 特判：`a%2MB==0 && pa%2MB==0 && 剩余>=2MB` → walkl1 装 L1 叶子。
  **L1 槽位已被占（如降级遗留的空 L0 表）→ 不 panic，回退普通页路径**（缩了又涨场景）。
  老调用者（size=PGSIZE）第三条件永假，完全透明。
- **`walkaddr` 偏移拼接（动工中抓到的大坑）**：超级页叶子的 `PTE2PA` 只给块基址，
  必须 `pa = 基址 | (va & (SUPERPGSIZE-1))` 模仿硬件拼低 21 位——否则
  loadseg/copyin/copyout 全指错地址。

**③ uvmalloc 三明治**：循环内先试三连条件 → superalloc + mappages(2MB) + `sz=SUPERPGSIZE`；
不满足/耗尽 → 落回普通页。堆起点不对齐时形成"零头普通页 + N 个超级页 + 零头"。

**④ 拆除与继承**：
- `uvmunmap`：`is_super(pa)` → 整块拆（校验 a 对齐且区间盖满，否则
  panic("partial superpage")）+ `superfree` + `sz=SUPERPGSIZE` 跨步。
  **每个 continue 前必须 `sz=PGSIZE` 重置步长**（否则跨 2MB 漏拆）。
- `uvmdealloc`：释放区间**两端**先 `demote_if_super()`。
- `demote_if_super`（拷贝降级）：walk 拿叶子 → is_super 为假直接 return（**幂等**）→
  摘 L1 叶子 → 512 圈 {kalloc + memmove 4KB + mappages}（第一圈 walk(alloc=1)
  自动铺新 L0 表）→ superfree 整块。**数据零丢失**（测试写 '9' 再切再读验证）。
- `uvmcopy`：is_super → superalloc 新块 + memmove 整 2MB + mappages 装 L1 叶子 +
  `szinc=SUPERPGSIZE`。耗尽 goto err（回滚 uvmunmap 已拷贝部分）。
  **不能父子共享块**：PTE 是 R|W，无 COW 机制拦写，共享=互踩（Lab 5 主题）。

### 3.3 为什么选拷贝降级而非原地降级

原地降级（512 个 PTE 直指原块切片）会让"预留区住着普通页映射"的特例扩散到
uvmunmap/uvmcopy/kfree 每处（切片不能 kfree 也不能 superfree，整块归还时机成谜）。
拷贝降级用一次 2MB 搬运买断全部复杂性：**降级完成即恢复"预留区外全是普通页"的纯净世界**。

### 3.4 五条不变量

1. 预留区地址只以完整 2MB 块身份出现在 PTE 里（uvmalloc/uvmcopy 三连条件保证）
2. `is_super(pa)` ⟺ 完整 L1 叶子（由 1 + 拷贝降级保证）
3. 预留区页永不进 4KB 空闲链表（kfree 入口 panic 防御）
4. 部分释放必先降级（uvmdealloc 两端 demote + uvmunmap panic 兜底）
5. 软件翻译 = 硬件语义（walkaddr 拼低 21 位）

**已知小低效（无害）**：释放区间起点若恰 2MB 对齐且后跟完整块，那次 demote 白搬
2MB（本可整块善终）。优化：demote 前先判"块是否完整落在释放区间内"。

---

## 4. 踩坑速查表

| 坑 | 症状 | 解法 |
|---|---|---|
| `fs.img` 不依赖内核 | 改内核后 `make fs.img` 说 up to date | 改内核必须 `make kernel/kernel` |
| `mv` 保留旧 mtime | 恢复文件后仍用旧 .o | `touch` 再 make |
| macOS 无 `timeout` | command not found | 后台运行 + sleep + 读日志 |
| `%p` 喂 uint64 | -Werror=format 编译失败 | 实参 `(void*)` 强转 |
| walkaddr 不拼偏移 | 超级页内 loadseg/copyin 指错地址 | `pa |= va & (SUPERPGSIZE-1)` |
| continue 不重置步长 | uvmunmap/uvmcopy 漏拆漏拷 | 每个 continue 前 `sz=PGSIZE` |
| 半拆超级页 | sbrk(-4K) 把整块拆了丢数据 | uvmdealloc 两端先 demote |
| 超级页 pa 被 kfree | 大块进 4KB 链表被双重认领 | kfree 入口 is_super → panic |
| 降级后再涨堆 | mappages 撞"remap" panic | L1 槽被占时回退普通页而非 panic |

---

## 5. answers-pgtbl.txt 三题答案要点

- **Q1**（print_pgtbl 逐条）：见 §2 实测输出解读表。
- **Q2**（还能加速谁）：只读小状态的 syscall——`sbrk(0)`（growproc 时同步
  usyscall->sz）、errno 缓存（glibc 实锤）、全局只读页放 uptime（Linux vDSO
  clock_gettime 同款）。**有副作用的（fork/read/exit/sbrk(n≠0)）必须留在陷入路径**；
  页是只读的，内核是唯一写者 → 不可伪造。
- **Q3**（vmprint 叶子对照）：8 个叶子与 print_pgtbl 同 va/pte/pa 一一对应；
  vmprint 额外展示**中间节点**（pte 尾数 ...801/...401/...001，V=1 且 RWX=0）
  和页表页自身的物理地址——这是 pgpte（只答叶子）永远给不出的视角。

---

## 6. 后续建议

- Lab 4 (`traps`)：backtrace（用 fp/ra 走内核栈）+ alarm（信号机制），
  复用 usertrap/trapframe 全部知识。
- Lab 5 (`cow`)：fork 共享页 + 写时复制——正是本 lab "uvmcopy 无脑深拷贝"
  的优化，PTE 清 W 位 + vmfault 里处理写保护 fault。superpg 的 uvmcopy
  改动届时需要 COW 化。
- 可选挑战：unmap 用户第 0 页（null deref 即杀）；PTE_D 汇报脏页 syscall。
