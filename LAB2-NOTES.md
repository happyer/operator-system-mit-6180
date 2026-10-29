# Lab 2 (`syscall`) 设计与调试笔记

> 分支 `syscall`，commit `63d40cb` + `da666f3`，`./grade-lab-syscall` = **45/45** ✅ 完结
> `answers-syscall.txt` 已做完（gdb 实测，见 §6）

---

## 0. ⚠️ 最重要的更正：2025 版实验二**不是** trace + sysinfo

`README.md` / `STUDY-PLAN.md` 里写的 "Lab 2 = trace + sysinfo" 是**错的**（那是 2023/24 版）。
2025 版 `grade-lab-syscall` 的真实构成：

| 任务 | 分值 | 状态 |
|---|---|---|
| `answers-syscall.txt`（gdb 问答，手动评） | 5 | ✅ 完成（实测数据见 §6） |
| `sandbox_mask` / `_fork` / `_path` / `_most` / `_minus` | 5×5=25 | ✅ OK |
| `attack` | 14 | ✅ OK |
| `time.txt` | 1 | ✅ OK（值 "20" 是占位，需按实际小时数改） |

**`trace` 不在评分范围内**，我们做它属于彩排（但它把 syscall 全链路练通了，`interpose` 复用了 80% 经验）。

官方讲义：https://pdos.csail.mit.edu/6.1810/2025/labs/syscall.html

---

## 1. 系统调用全链路（已彻底搞通）

### 1.1 谁执行了 `ecall`

用户程序调 `read(fd,buf,n)` 编译成 `jal 3d8 <read>`，而 `0x3d8` 处是
**make 由 `user/usys.pl` 生成的三行汇编桩**：

```asm
.global read
read:
 li a7, SYS_read     # 编号进 a7
 ecall               # 陷入
 ret                 # 返回值在 a0
```

- `user/user.h` 里 `int read(int,void*,int);` **只是声明**，全工程没有 C 实现
- 参数由**编译器**按调用约定放进 `a0`-`a2`，桩代码只补一个 `a7`
- 两层绑定：**链接期**（`read` 符号 → 0x3d8）+ **运行期**（`ecall` → `stvec`）

### 1.2 `ecall` 是"无目标跳转"

`ecall` 机器码 `0x00000073` **不含地址字段**。硬件只做 5 件事：

```
sepc ← pc;  scause ← 8;  sstatus.SPP ← 0;  SPIE ← SIE;  SIE ← 0;  特权级 ← S;  pc ← stvec
```

**硬件不碰 `sp`，不碰 `satp`，不碰 `ra`。** 这些全是软件（`uservec`）的活。
`csrw stvec` 是特权指令，U 态执行 → 非法指令异常 → 进程被杀。**用户对"陷阱去哪"零控制权。**

### 1.3 `uservec` 没有 caller

它由硬件 `pc ← stvec` 直接跳入。全工程对它的引用**没有一处是调用**
（只有 `.globl`/标签、`extern char uservec[]`、地址算术、`w_stvec`）。
它也没有 `ret`，只有 `userret` 末尾的 `sret`。

`trampoline.S` 的关键结构：

```asm
trampoline:
.align 4
.globl uservec
uservec:                 # ← 和 trampoline 同地址，偏移为 0
        csrw sscratch, a0    # 用 sscratch 当"第 33 个格子"腾出 a0
        li a0, TRAPFRAME
        sd ra, 40(a0) ... sd t6, 280(a0)   # 存 31 个寄存器（跳过 112=a0）
        csrr t0, sscratch; sd t0, 112(a0)  # 存回用户的 a0
        ld sp, 8(a0)      # kernel_sp
        ld tp, 32(a0)     # kernel_hartid
        ld t0, 16(a0)     # kernel_trap = usertrap
        ld t1, 0(a0)      # kernel_satp
        sfence.vma; csrw satp, t1; sfence.vma   # 换内核页表
        jalr t0           # 调 usertrap()
.globl userret
userret:                # ← 紧挨着！usertrap 返回后自然滑进来
        sfence.vma; csrw satp, a0; sfence.vma   # a0 = usertrap 的返回值 satp
        ... 恢复 31 个寄存器 ...
        csrr a0, sscratch
        sret
```

### 1.4 `stvec` 被写 3 次（全在 `trap.c`）

| # | 位置 | 值 | 时机 |
|---|---|---|---|
| ① | `trapinithart()` L29 | `kernelvec` | 开机，每 hart |
| ② | `usertrap()` L47 | `kernelvec` | 陷入内核后第一件事 |
| ③ | `prepare_return()` L111 | `TRAMPOLINE + (uservec - trampoline)` | 返回用户前 |

- `w_stvec()` = `csrw stvec, x`，**是写寄存器不是跳转**
- **scheduler 运行时 stvec 必为 `kernelvec`**（因为 `yield()` 在 `prepare_return()` 之前）
- `prepare_return()` 开头的 `intr_off()` 保护的正是"stvec 已=uservec 但人还在内核"这个窗口
- `TRAMPOLINE + (uservec - trampoline)`：链接地址 `0x80006000` ≠ 运行地址 `0x3ffffffff000`，必须换算

### 1.5 `usertrap()` 的 5 阶段 + 三分岔

阶段：① 安检 `SPP!=0 → panic` ② `w_stvec(kernelvec)` ③ 存 `epc` ④ 按 `scause` 分诊 ⑤ 收尾（killed/yield/prepare_return/return satp）

| `scause` | 含义 | 处理 |
|---|---|---|
| **8** | U 态 `ecall` | `epc += 4`（否则死循环）→ `intr_on()` → `syscall()` |
| 最高位=1 | 中断 | `devintr()`；返回 2 = 时钟 → `yield()` |
| **13 / 15** | 取指/读写页错误 | `vmfault()` 懒分配 |
| 其他 | 非法指令等 | `setkilled(p)` |

### 1.6 `prepare_return()` 有**两个**调用者（先有鸡问题的答案）

| 调用者 | 场景 |
|---|---|
| `usertrap()` L87 | 常规：每次陷阱返回前 |
| **`forkret()` (proc.c:533)** | ⭐ 每个进程**首次**进用户态 |

`forkret` 靠 `allocproc()` 里预设的 **`p->context.ra = (uint64)forkret`** 被 scheduler 的
`swtch` "返回"进来（伪造出"曾调用过 swtch"的假象）。然后：

```c
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);   // 手动伪造对 userret 的调用
```

注释原话："mimicing usertrap()'s return"。`userinit()` 里**不能**直接 `kexec("/init")`，
因为 `fsinit()` 内部要 `sleep()`，必须在进程上下文里跑。

---

## 2. `interpose` 设计（25 分）

### 2.1 语义

```c
int interpose(int mask, char *path);
```
- `mask` 的 **bit N = 1 表示"拒绝"syscall N**（黑名单；和 `trace_mask` 语义相反）
- `mask = 0` = 完全自由 → **天然的好默认值**（`allocproc` 清零即可）
- `path` = 被屏蔽的 `open()` 仍可使用的那**一个**路径；`"-"` 实际上永不匹配任何路径 = 无豁免
- **必须被 fork 继承**（否则 fork 一下就逃逸）；**exec 后必须保留**（否则 exec 一下就自由）

### 2.2 ⭐ 讲义与测试矛盾：只有 `open` 享受路径豁免

讲义说 "If **open or exec** is masked but the pathname matches ... should be allowed"。
但 `sandbox_path` 最后一条 `'sandbox 128 sh sh < exec.sh'` 断言
`re.findall("exec ls failed") == 0`：

- 若 exec 也豁免 → `exec("sh")` 匹配 path `"sh"` → 放行 → sh 跑起来 →
  第二行 `ls` 的 `exec("ls")` 被拒 → sh 打印 `exec ls failed` → **断言失败**
- 若 exec 不豁免 → `sandbox: exec sh failed`，sh 从未运行 → **两个断言都过** ✓

**结论：以测试为准，只豁免 `open`。实测 `sandbox_path: OK` 证实。**

### 2.3 5 项测试各自考什么

| 测试 | 命令 | 考点 |
|---|---|---|
| `sandbox_mask` | `sandbox 32768 - cat README` | 基本屏蔽（`32768=1<<15=open`，`128=1<<7=exec`）|
| `sandbox_fork` | `sandbox 32768 - sh < exec.sh` | **fork 继承**：`echo` 能跑（不需 open）、`ls` 挂 |
| `sandbox_path` | `sandbox 32768 README grep xv6 x` | **路径豁免**：断言 `xv6` 恰好 12 次 = 1(横幅)+3(回显)+4+4 |
| `sandbox_most` | `sandbox 2147483643 grep ...` | 除 `exit`(bit2) 全禁 → 连 `write` 都被禁 → **报错都打不出来** |
| `sandbox_minus` | `sandbox 32768 --- grep hello! ---` | 路径名长成 `---` 也要按文件名精确匹配 |

`exec.sh` 内容（lab 提供）：`echo "hello world!"` + `ls`

### 2.4 8 处改动的选址理由

| 层 | 文件 | 为什么必须是这里 |
|---|---|---|
| 接线 | `Makefile` `$U/_sandbox` | UPROGS 是"进 fs.img"的唯一清单；缺了 → 运行时 `exec sandbox failed` |
| 接线 | `user/user.h` | 编译器要声明；缺了 → `implicit declaration` |
| 接线 | `user/usys.pl`（**不是** `usys.S`，后者是生成物）| 生成那三行汇编实体；缺了 → `undefined reference` |
| 接线 | `kernel/syscall.h` `SYS_interpose 23` | 用户/内核**唯一的共享契约**（22 被 trace 占了）|
| 存储 | `kernel/proc.h` `int imask; char ipath[MAXPATH];` | 每进程属性；放 "private" 区（无需 `p->lock`，天然串行）；**必须是数组不能是指针**（用户会改/会 free/内核页表没映射）。`MAXPATH` 来自 `param.h`，所有 .c 都先 include 它 |
| 继承 | `kernel/proc.c` `kfork()` | 全工程**唯一**创建 proc 的地方 |
| 写入 | `kernel/sysproc.c` `sys_interpose()` | xv6 惯例：`syscall.c` 只分发，进程相关实现归 `sysproc.c` |
| 执行 | `kernel/syscall.c` `syscall()` | ⭐ 三条硬约束：**唯一咽喉**（Reference Monitor 不可绕过）+ **num 已过三重检查**（`1<<num` 才有定义）+ **在 dispatch 之前**（被拒调用零副作用）|

**不该改的地方**：`usertrap()`（还没取 a7，且会误伤中断/缺页）、各 `sys_xxx()`（22 处必漏）、
`usys.S` 桩（用户态不可信，`li a7,15;ecall` 就绕过）、`kexec()`（清 mask 会让沙箱形同虚设）。

### 2.5 核心代码

```c
static int
interposed(struct proc *p, int num)
{
  char path[MAXPATH];
  if((p->imask & (1 << num)) == 0)
    return 0;                       // 没被屏蔽
  if(num == SYS_open){
    if(argstr(0, path, MAXPATH) >= 0 &&
       strncmp(path, p->ipath, MAXPATH) == 0)
      return 0;                     // 白名单路径，豁免
  }
  return 1;                         // 拒绝
}
```
`syscall()` 里：三重检查之后、dispatch 之前插入
`if(interposed(p,num)){ p->trapframe->a0 = -1; return; }`
（`return` 而非继续，被拒的调用不该进 trace 日志）。

### 2.6 `interposed()` 对**每一次** syscall 都执行

包括 `interpose` 自己那次 —— 它不被挡是因为当时 `p->imask` 还是 0。
推论：**若 mask 含 bit 23，进程就再也改不了自己的沙箱**（不可逆性自动实现）。
`sandbox` 的**父**进程 `imask` 恒为 0（先 fork 再 interpose），所以父进程能正常 `wait`。

### 2.7 已知设计缺陷

`p->imask = mask`（直接赋值）**可逆** —— 被攻破的程序调 `interpose(0,"-")` 就能解套。
生产级应该用 `p->imask |= mask`（只能越来越严）。按讲义/官方测试用了直接赋值。

---

## 3. `attack` 设计（14 分）

### 3.1 漏洞本体：三行 `memset` 被 `#ifndef LAB_SYSCALL` 关掉

| # | 位置 | 被关掉的代码 |
|---|---|---|
| 1 | `kalloc.c:57-60`（`kfree`）| `memset(pa, 1, PGSIZE)` |
| 2 | `kalloc.c:86-89`（`kalloc`）| `memset((char*)r, 5, PGSIZE)` |
| 3 | `vm.c:262-264`（`uvmalloc`）| `memset(mem, 0, sz)` |

→ 进程死后物理页内容原封不动，下个进程可能拿到"脏页"。
对应 **CWE-226**（资源复用前未清除敏感信息）；真实案例 **Heartbleed**。

**功能正确 ≠ 安全**：讲义明说 "it even passes most of usertests"。安全 bug 让所有测试都绿。

### 3.2 ⭐ 生死攸关：必须 `sbrk()`，不能 `sbrklazy()`

2025 版 `sys_sbrk` 有**两个参数** `(int n, int t)`：

```c
char *sbrk(int n)     { return sys_sbrk(n, SBRK_EAGER); }   // ulib.c:152
char *sbrklazy(int n) { return sys_sbrk(n, SBRK_LAZY);  }   // ulib.c:157
```

| 路径 | 走到哪 | 清零？ | 能偷到？ |
|---|---|---|---|
| `sbrk` (EAGER) | `growproc` → `uvmalloc` | ❌ memset 被关掉 | ✅ **脏页** |
| `sbrklazy` (LAZY) | 只加 `p->sz`，用时 `vmfault` | ✅ `vmfault` 的 memset **没有 `#ifndef`** | ❌ 全 0 |

### 3.3 `secret.c` 留的路标

```c
#define DATASIZE (8*4096)
char data[DATASIZE];
strcpy(data, "This may help.");   // offset 0：路标
strcpy(data + 16, argv[1]);       // offset 16：秘密
exit(0);                          // 8 页被 kfree，内容不变
```

### 3.4 ⭐ `(null)` 调试实录（本次最有价值的经验）

**症状**：第一次 `attack` 打印 `(null)`，第二次才打印真秘密。

**排查**：
1. 查 `(null)` 来源 → `user/printf.c:99-100`：`if((s = va_arg(ap,char*)) == 0) s = "(null)";`
   → 只有指针字面为 0 才会打印，但 `p+16` 不可能是 0 → **假设错了**
2. 加诊断输出 dump 十六进制 →
   ```
   hit#1 off=67872  p+16: 28 6E 75 6C 6C 29 00  = "(null)"      ← 假命中
   hit#2 off=69648  p+16: 53 45 4B 52 49 54 31 32 33 00        ← 真命中
   ```
3. **真相：路标在内存里出现两次**
   - `.rodata`：`strcpy` 的**源字面量**本身；紧跟其后是链接进来的 `printf.o` 的 `"(null)"`（正好 +16）
   - `.bss`：`strcpy` 的**目的地** `data[]`；+16 处是真秘密

**修正**：用讲义给的约束过滤 —— "secret 只含数字和大小写字母"：

```c
static int is_secret(char *s){   // 全字母数字且非空、长度 ≤ 32 才算
  for(int n=0; n<SECRETMAX; n++){
    char c = s[n];
    if(c=='\0') return n>0;
    if(!((c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z'))) return 0;
  }
  return 0;
}
```
`"(null)"` 的括号、`"attack: sbrk failed"` 的冒号空格、`"%s"` 的百分号全被拒 ✓

> 💡 教训：**内存扫描攻击面对的是目标进程的全部足迹**（代码段/rodata/字符串常量池），
> 不只是"目标数据"。真实攻击（Heartbleed 利用）同样要处理这种噪声。

### 3.5 其他实现要点

- `CHUNK = 512 * PGSIZE`（2MB）：secret 只释放约 12 页，但 freelist 是 **LIFO** 且中间有
  shell fork/exec 搅动，多要才能稳定覆盖 → 两次 attack 都成功
- `*p == 'T'` 前置过滤：只有 ~1/256 位置可能匹配，省掉 200 万次 14 字节 `memcmp`
- 循环留 **96 字节余量**：保证 `is_secret()` 不会读越界（越界 → 缺页 → `vmfault` 因
  `va >= p->sz` 返回 0 → `setkilled` → 进程被杀）
- 打印**所有**通过过滤的候选（不只第一个），最大化命中率

---

## 4. 踩过的坑（速查）

| 坑 | 症状 | 解法 |
|---|---|---|
| **内核没有 `strcmp`** | 链接 undefined | 内核只有 `strncmp`（`string.c:60`）；**用户态相反**：有 `strcmp`/`memcmp`，**没有** `strncmp` |
| **`make` 不建用户程序** | exit 0 但 `user/_sandbox` 不存在 | Makefile **没有 `all:` 目标**，默认只建 `kernel/kernel` → 必须 **`make fs.img`** |
| **管道吞掉退出码** | `make \| tail` 永远 exit 0 | 用 `make > log 2>&1; echo $?`，再 grep log |
| **`-Werror` + `%d`** | `uint64` 传给 `%d` 编译失败 | 显式 `(int)p->trapframe->a0` |
| **grep 匹配到 `-Werror`** | 满屏假"error" | 用 `grep -E "error:\|Error [0-9]\|undefined reference"` |
| **QEMU 管道输入太快** | Ctrl-A X 提前退出，测不到输出 | `( sleep 5; echo cmd; sleep N; printf '\001x' ) \| make qemu > log` |
| **`1 << num` UB** | num 为负或 ≥32 | 拦截逻辑必须放在三重边界检查**之后** |
| **`fetchaddr` 双重检查** | — | `addr>=p->sz` 和 `addr+8>p->sz` **都要**，否则 `addr+8` 溢出回绕骗过检查 |
| **`fs.img` 不依赖内核** | 改了 `kernel/*.c` 后 `make fs.img` 说 "up to date" | Makefile 里 `fs.img` 只依赖 `mkfs + README + UPROGS`，**改内核必须 `make kernel/kernel`** |
| **`mv` 保留旧 mtime** | 恢复文件后 make 仍用旧的 .o | `mv backup orig` 后 orig 的时间戳是备份时刻的 → 必须 `touch` 再 make，否则增量编译被跳过 |

---

## 5. 相关机制速查（这几天讲透的）

### 5.1 位图（bitmap）
`mask & (1<<n)` 测试 / `|= (1<<n)` 打开 / `&= ~(1<<n)` 关闭。
同一模式在 xv6 里到处都是：`PTE_V|PTE_R|PTE_W|PTE_X|PTE_U`（`mappages` 的 `perm`）、
`O_CREATE|O_WRONLY`、`sstatus` 的 SPP/SPIE/SIE、`sie` 中断使能。
`1` 是 `int`（32 位）→ `1<<31` 及以上是 UB。syscall >32 个就得换 `uint64` 或数组（Linux 用 BPF）。

### 5.2 页表类型
```c
typedef uint64 pte_t;           // 一个页表项（8 字节）
typedef uint64 *pagetable_t;    // 一张页表（4096 字节 = 512 个 pte_t）
```
`_t` = type。`PTE_V` = bit 0 = valid。
**V=1 不代表是叶子**：`R|W|X` 全 0 = 中间节点（指向下一级页表），非 0 = 叶子（指向物理页）。
见 `freewalk()`：`(pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X))==0` → 递归；否则 `panic("leaf")`。

### 5.3 `walk()` vs `walkaddr()` vs `mappages()`
| 函数 | 方向 | 返回 |
|---|---|---|
| `kalloc()` | 无 → PA | 一块新物理页 |
| `mappages(pt,va,sz,pa,perm)` | **写**（建映射）| 0/-1；**内部自己 `walk(pt,a,1)`**，调用者不需要先 walk |
| `walk(pt,va,alloc)` | **读**（找格子）| `pte_t*` = **格子的地址**，不是物理地址 |
| `walkaddr(pt,va)` | **读**（翻译）| 页对齐的物理地址（页内偏移调用者自己加）|

`mappages` 的三个 panic 前置：va 页对齐、size 是整页倍数、size≠0；`*pte & PTE_V` → `panic("remap")`。
`walk` 返回非 0 **不代表映射存在**，必须再查 `*pte & PTE_V`（`walkaddr` L135 就是这么做的）。

### 5.4 内核态 ≠ 能直接解引用用户指针
`syscall()` 时特权级 S + 内核页表 + 内核栈**全都到位**，但**用户 VA（如 0x1234）在内核页表里没映射**
→ 直接解引用会缺页 → `kerneltrap` → panic。
`copyin`/`copyout` 的两步妙手：
```c
pa0 = walkaddr(pagetable, va0);              // ① 用【用户页表】翻译
memmove(dst, (void*)(pa0 + (srcva-va0)), n);  // ② 物理地址直接当内核 VA 用（恒等映射）
```
② 成立是因为 `kvmmap(etext, etext, PHYSTOP-etext, R|W)` 把**全部物理内存恒等映射**了。
参数搬运工：`argraw`(从 trapframe 取 a0-a5，最多 6 个参数) / `argint` / `argaddr`(只取数字不校验) /
`fetchaddr` / `fetchstr` / `argstr`(= argaddr + fetchstr)。

### 5.5 `syscall()` 的三重防御
`num > 0`（防负数越界）、`num < NELEM(syscalls)`（防大数越界）、`syscalls[num]`（防表里空洞=NULL）。
`a7` 完全由用户控制，漏任一条都是可提权漏洞。`num` 是 `int` 而 `a7` 是 `uint64` → 有隐式截断。
else 分支只打印 + `a0=-1`，**不 panic 不杀进程**（用户传错号是用户的 bug，内核要健壮）。

### 5.6 `mappages` 的 6 个调用者
`kvmmap`(开机×7) / `proc_pagetable`(蹦床页+trapframe，**都不给 PTE_U**) /
`uvmalloc`(sbrk) / `uvmcopy`(fork) / `vmfault`(缺页懒分配) / `allocproc`(内核栈)。

---

## 6. ✅ 已完成：`answers-syscall.txt`（5 分，本机 gdb 实测）

### 6.1 已能回答的 3 题

1. **backtrace 显示谁调用了 `syscall()`？** → `usertrap()`，`kernel/trap.c:68`
   （全内核唯一调用点；`defs.h:137` 只是声明，`kernel.asm` 是反汇编）
2. **`p->trapframe->a7` 的值和含义？** → 系统调用号。`init` 的第一个调用是 `exec` → **7**
   （`user/init.c`：`exec("init", argv)`，其 `user/init.asm` 里能看到 `li a7,7`）
3. **`p /x $sstatus` 看出的"之前的模式"？** → **U 态**（`SPP` 位 = 0；`sstatus` 值形如 `0x...20`，
   SPP 是 bit 8）

### 6.2 实做的 3 题（`num = *(int*)0` 实验，2026-10-15 实测）

步骤：
```bash
# 1) 临时把 kernel/syscall.c 的 num = p->trapframe->a7; 改成 num = *(int*)0;
# 2) make kernel/kernel && make qemu-gdb   （一个终端；注意端口是随机的，本机为 25501）
# 3) 另一个终端：
riscv64-unknown-elf-gdb -batch -ex 'target remote localhost:25501' \
  -ex 'b *0x80001cfc' -ex 'c' -ex 'x/2i $pc' \
  -ex 'p p->pid' -ex 'p p->name' -ex 'p p->state' kernel/kernel
# 4) gdb detach 后 QEMU 继续执行，panic 输出在 qemu-gdb 控制台日志里
# 5) 恢复源码：touch kernel/syscall.c && make kernel/kernel
#    （❗ mv 备份回去会保留旧 mtime，make 会跳过重编，必须 touch）
```

**实测结果**：
- panic 打印：`scause=0xd sepc=0x80001cfc stval=0x0` → `panic: kerneltrap`
- `kernel.asm` 在 `0x80001cfc`：`lw s2,0(zero)` —— **`num` 在 `s2` 寄存器**（讲义示例是
  `0x80001bfe`/其它寄存器，每台机器编译结果不同，必须实测）
- gdb 断点现场：`p->pid = 1`，`p->name = "init"`，`p->state = RUNNING`

答案要点：
4. **出错的汇编指令** = `lw s2,0(zero)`（从 zero 寄存器算出的地址 0 处加载一个字）；
   **`num` 对应 `s2`**
5. **为什么崩**：地址 0 在内核页表里**没有映射**（内核只映射 `KERNBASE` 以上 + 设备 MMIO +
   TRAMPOLINE）→ load page fault → `scause = 0xd`（13）→ `kerneltrap()` 里
   `devintr()` 返回 0 → `panic("kerneltrap")`。这正是 `copyin/copyout` 存在的原因。
6. **进程名 / pid** = **`init` / pid 1**（它是内核启动的第一个用户进程，其第一次
   syscall（exec）就是注入 bug 后的第一个受害者）

### 6.3 文件格式

纯文本，每题一段。`check_answers()` 只检查文件存在且非空（讲义说 "will be graded manually"）。

---

## 7. 后续建议

1. `time.txt` 现在是占位 `"20"`，按实际小时数改（讲义要求单个整数）
2. 补完 `answers-syscall.txt` → `./grade-lab-syscall` 应达 **45/45**
3. 更正 `README.md` L71 和 `STUDY-PLAN.md` L105-106/406-409/706 里
   "Lab 2 = trace + sysinfo" 的错误描述
4. Lab 3 `pgtbl` 会重度用到本文 §5.2/§5.3 的页表知识（`walk`/`mappages`/`PTE_LEAF`），
   以及 `vm.c:105-109` 那个 `#ifdef LAB_PGTBL` 的大页分支
