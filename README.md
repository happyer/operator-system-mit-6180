# MIT 6.1810 (xv6-labs-2025) 学习环境

本目录是 MIT 6.1810《Operating System Engineering》的本地开发环境，包含 **xv6 源码**、**RISC-V 交叉工具链**、**QEMU** 和 **官方教材**。

---

## 一、目录结构

```
mit61810/
├── README.md                   # 本文件
├── env.sh                      # 环境激活脚本（source 一下就能用）
├── setup-xv6-env.sh            # 一键搭建脚本（换电脑时用）
├── docs/                       # 官方教材
│   ├── book-riscv-rev5.pdf           # xv6 教材（主教材，rev5）
│   └── xv6-src-booklet-rev5.pdf      # xv6 源码小册（92 页，打印友好）
├── tools/                      # 工具（已装好，开箱即用）
│   ├── xpack-riscv-none-elf-gcc-14.2.0-3/   # RISC-V GCC 14.2.0 + GDB（含参数注入 wrapper）
│   ├── qemu-local/                            # 本地编译的 QEMU 11.1.2（仅 riscv64-softmmu）
│   ├── venv/                                  # 专供 make grade 使用的 python 环境（含 pexpect）
│   ├── build-qemu-minimal.sh                  # QEMU 编译脚本（可重复执行）
│   └── qemu-build/qemu-11.1.2/                # QEMU 源码 + 编译中间产物（约 1GB，可安全删除）
└── xv6-labs-2025/              # xv6 源码（当前 net 分支，九个 Lab 已完成）
```

---

## 二、快速开始

```bash
# 1) 激活环境（每次开新终端都要执行一次）
source /Users/a1/Downloads/mit61810/env.sh

# 2) 进入 xv6 源码目录
cd /Users/a1/Downloads/mit61810/xv6-labs-2025

# 3) 编译并启动 xv6
make qemu
```

启动后会进入 xv6 的 shell，你可以敲 `ls`、`echo hello`、`cat README`：

```
xv6 kernel is booting
...
$ ls
.              1 1 1024
..             1 1 1024
README         2 2 2059
...
$ 
```

**退出 xv6：先按 `Ctrl-A`，松开后再按 `X`**（注意是大写 X）。

跑评分脚本（Lab 1 共 15 项、131 分）：

```bash
make grade
```

---

## 三、Lab 工作流

9 个 Lab 分别对应一个 git 分支（2025 版无 thread）：

| Lab | 分支 | 主题 |
|-----|------|------|
| 1 | `util` | Unix 工具（sleep、sixfive、memdump、find、xargs）✅ 131/131 |
| 2 | `syscall` | 系统调用（sandbox/interpose 沙箱、attack 内存残留攻击、gdb 问答）⚠️ 2025 版无 trace/sysinfo ✅ 45/45 |
| 3 | `pgtbl` | 页表（ugetpid 共享页、vmprint 打印页表树、2MB 超级页）⚠️ 2025 版无 sys_pgaccess ✅ 41/41 |
| 4 | `traps` | 陷阱（backtrace 栈帧链、alarm 劫持返程票）✅ 95/95 |
| 5 | `cow` | 写时复制 fork ✅ 130/130 |
| 6 | `lock` | 锁与并发（kalloc per-CPU freelist、读写锁 rwspinlock）✅ 100/100 ⚠️ 2025 版无 thread lab（旧版 buffer cache 分桶也已改为 rwlock） |
| 7 | `fs` | 文件系统（large files 双间接块、symlinks）✅ 100/100 |
| 8 | `mmap` | 内存映射文件（VMA + fault 供页 + 脏页回写）✅ 170/170 |
| 9 | `net` | E1000 网卡驱动 + UDP 端口队列（bind/recv/unbind）✅ 171/171；commit `039fa90`，实际用时 4h |

**2026-10-29：九个 Lab 全部完成，累计 983/983 分（各分支历史验收记录合计）。**

切换 Lab：

```bash
git stash / git checkout <你的分支名>   # 先把当前改动保存或提交
git checkout syscall                     # 切到 Lab 2
```

`conf/lab.mk` 里的 `LAB=` 决定 `make grade` 跑哪个评分脚本，切分支时会自动更新。切换后先 `make clean` 再 `make qemu`。

### Lab 1（`util`）任务清单

`util` 分支是**空白起始代码**：`sleep` / `sixfive` / `memdump` / `find` / `xargs` 都还不存在，
需要你自己实现，并**把 `$U/_xxx` 加进 `Makefile` 的 `UPROGS`**（否则不会被打包进 `fs.img`）。
当前 `make grade` 得 **0/131**，属于正常起始状态。

| 分值 | 测试项 | 要做的事 |
|------|--------|----------|
| 5+5+10 | sleep ×3 | 写 `user/sleep.c`（无参数要报错），加进 UPROGS |
| 10+10+10 | sixfive ×3 | 写 `sixfive`：把输入里的 6→9、5→4（需读 `user/sixfive.txt` 验证） |
| 10+10 | memdump ×2 | 写 `memdump`：把内存 dump 成十六进制 |
| 10+10+10 | find ×3 | 写 `find`：支持子目录、递归查找 |
| 10+10+10 | exec ×3 | 给 `find` 加 `-exec`，并写 `xargs` |
| 1 | time | 在 `time.txt` 里写下你花的小时数 |

每跑完一次 `make grade`，`xv6.out*` 里会保留各测试的完整串口输出，方便调试。

常用调试方式：

```bash
make qemu-gdb        # 在另一个终端: riscv64-unknown-elf-gdb kernel/kernel -ex 'target remote localhost:26000'
```

**提示**：`make grade` 用的是 Python 脚本（`grade-lab-*`），需要 `python3` + `pexpect`。
已建好专用 venv（`tools/venv/`），`source env.sh` 后 `python3` 会自动指向它。

---

## 四、这个环境是怎么装的（以及为什么这么做）

### 1. RISC-V 工具链：用 xPack 预编译包，而不是 Homebrew

课程官方推荐 `brew tap riscv/riscv && brew install riscv-tools`。但：

- Homebrew 的这个 formula **只在 macOS ≥ 15（Sequoia）提供预编译包**；
- 本机是 **macOS 14（Sonoma）**，会**从源码编译**，体积约 **6.5 GB**、耗时数小时。

所以改用 **xPack 预编译工具链**（`xpack-riscv-none-elf-gcc-14.2.0-3`，约 316 MB），几分钟装好，功能完全够用（gcc / g++ / ld / objcopy / objdump / **gdb** 齐全）。

但它和课程默认的 `riscv64-unknown-elf-gcc` 有两处差异，脚本已自动补齐：

| 差异 | xPack 默认 | xv6 需要 | 处理方式 |
|------|-----------|---------|---------|
| 二进制名前缀 | `riscv-none-elf-` | `riscv64-unknown-elf-`（Makefile 自动探测） | 建立同名符号链接 |
| 默认架构/ABI | `rv32i` / `ilp32`（软浮点，无 `zicsr`） | `rv64gc` / `lp64d` | 用 wrapper 注入 `-march=rv64gc -mabi=lp64d` |
| 链接器默认模拟 | `elf32-littleriscv` | `elf64-littleriscv` | 用 wrapper 注入 `-melf64lriscv` |

wrapper 长这样（位于 `tools/.../bin/riscv64-unknown-elf-gcc`）：

```sh
#!/bin/sh
exec ".../riscv-none-elf-gcc" -march=rv64gc -mabi=lp64d "$@"
```

**这样做的最大好处：完全不修改 xv6 源码**，`git diff` 里永远只有你自己写的实验代码，方便交作业和对照。

### 2. QEMU：本地编译「最小化」版本，而不是 `brew install qemu`

`brew install qemu` 在本机是**最慢的一条路**：macOS 14 上 Homebrew 没有 qemu 的预编译包，
会连带从源码编译 12+ 个依赖（gnutls / libssh / capstone / ncurses / libusb / vde / dtc / meson / …），
预计 1 小时以上。

而 xv6 真正需要的只有 `qemu-system-riscv64` 一个可执行文件。所以用 `tools/build-qemu-minimal.sh`
编译了一个**只含 riscv64-softmmu** 的 QEMU：

- 关闭全部可选功能：gtk / cocoa / sdl / vnc / curses / opengl / capstone / gnutls / libssh / slirp / vde / hvf …；
- libfdt(dtc) 直接使用 QEMU 源码包内置的 subproject，无需安装 Homebrew 的 dtc；
- 只依赖本机已有的 glib / pixman / ninja / pkgconf / python3；
- 实测**约 2 分钟**编译完成（源码已解压在 `tools/qemu-build/`）。

产物：`tools/qemu-local/bin/qemu-system-riscv64`（版本 11.1.2，≥ xv6 要求的 7.2）。
`env.sh` 已把它加入 `PATH`，所以 `make qemu` 可以直接用。

---

## 五、常见问题

**Q: `make qemu` 报 `qemu-system-riscv64: command not found`？**
A: 没 `source env.sh`，或 QEMU 还没装完。

**Q: 报 `Couldn't find a riscv64 version of GCC/binutils`？**
A: `TOOLPREFIX` 探测失败，通常是 `tools/*/bin` 不在 `PATH`。执行 `source env.sh` 后确认 `command -v riscv64-unknown-elf-gcc` 有输出。

**Q: 汇编报 `unrecognized opcode 'csrr ...', extension 'zicsr' required`？**
A: wrapper 丢了（`tools/.../bin/riscv64-unknown-elf-gcc` 被恢复成普通符号链接了）。重新执行 `bash setup-xv6-env.sh` 即可。

**Q: 链接报 `target emulation 'elf64-littleriscv' does not match 'elf32-littleriscv'`？**
A: 同上，`riscv64-unknown-elf-ld` wrapper 丢失。

**Q: QEMU 版本太老（xv6 要求 ≥ 7.2）？**
A: 当前用的是 `tools/qemu-local/bin/qemu-system-riscv64`（11.1.2），已满足要求。

**Q: `make grade` 报 `No module named 'pexpect'`？**
A: 忘了 `source env.sh`。grade 脚本是 Python 写的并依赖 `pexpect`；已在 `tools/venv/` 里装好，
`env.sh` 会把该 venv 的 `python3` 放在 `PATH` 最前面（因此不会污染系统 Python）。

**Q: 不想等 Homebrew 编译 QEMU？Docker 备用方案**
A: 用 Ubuntu 容器（Ubuntu 官方源里有 `gcc-riscv64-linux-gnu` 和 `qemu-system-misc` 的 arm64 包）：

```bash
docker run -it --rm -v /Users/a1/Downloads/mit61810/xv6-labs-2025:/xv6 -w /xv6 ubuntu:24.04 bash
# 容器内：
apt update && apt install -y build-essential gcc-riscv64-linux-gnu qemu-system-misc gdb-multiarch python3 git
export TOOLPREFIX=riscv64-linux-gnu-
make qemu
```
（容器方案也不需要本目录的 `tools/`，用系统自带的 `riscv64-linux-gnu-` 前缀，xv6 的 Makefile 会自动识别。）

**Q: 如何彻底重装？**
A: `rm -rf tools env.sh && bash setup-xv6-env.sh`

---

## 六、参考资源

- 课程主页：https://pdos.csail.mit.edu/6.1810/2025/
- Lab 列表与说明：https://pdos.csail.mit.edu/6.1810/2025/labs/
- xv6 教材（在线）：https://pdos.csail.mit.edu/6.1810/2025/xv6/book-riscv-rev5.pdf
- xv6 源码仓库：`git://g.csail.mit.edu/xv6-labs-2025`
- 官方工具安装说明：https://pdos.csail.mit.edu/6.1810/2025/tools.html

> 建议阅读顺序：教材第 1 章（Operating system interfaces）→ 第 2 章（OS organization）→ 边做 Lab 1 边读第 1、2 章代码（`kernel/main.c`、`kernel/proc.c`、`user/`）。

---

_本环境于 macOS 14.8.5 (Apple Silicon) 上搭建并**完整验证通过**：_

- _RISC-V GCC 14.2.0 (xPack) 编译内核 `kernel/kernel`（268K，ELF64 / RISC-V）与全部用户程序 ✓_
- _`fs.img` 生成成功（2.0M）✓_
- _QEMU 11.1.2 启动 xv6：`xv6 kernel is booting` → `init: starting sh` → 执行 `echo` / `cat README` 输出正确 ✓_
- _`make grade` 评分框架跑通：15 项测试全部执行、逐项对比并生成报告（0/131，因实验代码尚为空）✓_
