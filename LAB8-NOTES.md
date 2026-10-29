# LAB 8 · mmap 蓝图（170 分）

> 分支 `mmap`，base `5648f64`。从零实现 mmap/munmap + fault 驱动文件页 + VMA 管理。

## 0. 侦察结论（已实锤）

- syscall 号停在 21 → **SYS_mmap=22, SYS_munmap=23**；接线五件套全要自己接（syscall.h/syscall.c/usys.pl/user.h/defs.h）
- **vmfault 已存在**（vm.c:453）：现版只服务 sbrk 懒分配（`va >= p->sz` 直接 return 0）；trap.c:71-73 已挂好 scause 13(load fault)/15(store fault) 入口 → **要扩展它**：va 落在 VMA 里时走"文件页供给"路径
- proc.h/file.h **无任何 VMA 骨架**——全部从零设计
- Makefile UPROGS 已含 _mmaptest（LAB=mmap 段）✓
- 测试 = mmap_test + fork_test + more_test 三函数（448 行），考点：
  1. basic: 映射已知内容文件(2页)校验 + munmap
  2. private: MAP_PRIVATE 写不回文件（重读文件确认原样）
  3. read-only: 只读 fd + PROT_WRITE|MAP_SHARED 必须 mmap 失败
  4. read/write: SHARED 可写映射 + **close(fd) 后映射仍有效** + dirty 写回落盘验证
  5. not-mapped unmap: munmap 中间一页（**VMA 要分裂**）
  6. lazy access: mmap 后**不得立即读盘**（unlink 掉文件名后照样能 fault 供页 → file refcount 保命）
  7. two files: 同时映射两个文件（多 VMA）
  8. fork: 子进程继承映射可访问；子进程 munmap 后再访问必须死（fault 杀）
- mmaptest 全部传 addr=0（内核自选地址）

## 1. 数据结构

```c
// proc.h
struct vma {
  int used;
  uint64 start, end;     // [start, end) 页对齐
  int prot, flags;       // PROT_READ/WRITE, MAP_SHARED/PRIVATE
  struct file *f;        // filedup 过的引用
  uint64 offset;         // 文件内起始偏移
};
// struct proc 里加: struct vma vmas[16];  (allocproc/freeproc 清零)
```

## 2. sys_mmap(addr, len, prot, flags, fd, offset)

1. 校验: addr 必须是 0(测试全传0); len 上取整 PGSIZE; fd 有效且 f->readable/writable 匹配 prot:
   - **PROT_WRITE 且 MAP_SHARED 且 !f->writable → 返回 -1**（考点3）
2. 找空闲地址区间: 从高往低（起点 TRAMPOLINE 下方安全边距），避开所有现有 VMA 与 [0, p->sz)；找不到 → -1
3. 找空 VMA 槽登记 {start,end,prot,flags,f=filedup(f),offset}；无槽 → -1
4. **不读盘不映射任何页**（懒！考点6），返回 start
- 注意: fd 无效/f->type!=FD_INODE → -1

## 3. vmfault 扩展（trap.c 已接好入口）

```c
uint64 vmfault(pagetable, va, read) {
  va = PGROUNDDOWN(va);
  // ① 原 sbrk 懒分配路径: va < p->sz → 现有逻辑不动
  // ② 新增: 扫描 p->vmas 找 va ∈ [start,end) 的 VMA
  //    - 没找到 → return 0 (非法访问, trap.c 杀进程)
  //    - 权限检查: read fault 要 PROT_READ; write fault 要 PROT_WRITE, 否则 return 0
  //    - kalloc + memset 0
  //    - readi(v->f->ip, 0, mem, v->offset + (va - v->start), PGSIZE)
  //      返回值 < PGSIZE = 文件短(EOF后), 剩余保持零 ✓
  //    - mappages 权限 = PTE_U | (prot&R→PTE_R) | (prot&W→PTE_W)
  //      (PRIVATE 也给 PTE_W——写自己的副本, 只是永不回写)
  //    - 返回 mem
}
```
并发: fault 路径全程在 trap 上下文(myproc 有效); VMA 扫描无需额外锁(单进程自己的)。

## 4. sys_munmap(addr, len)

1. addr 页对齐; 找覆盖 [addr, addr+len) 的 VMA; 无 → -1
2. 逐页处理 [addr, min(addr+len, v->end)):
   - walk 查 PTE: 若已映射且 **PTE_D 置位且 SHARED 且可写** → writei 回写文件对应偏移, 清 D
   - uvmunmap 该页
3. VMA 裁剪/分裂:
   - munmap 覆盖头部 → v->start += len (offset 同步 += len)
   - 覆盖尾部 → v->end = addr
   - **中间挖洞 → 分裂**: 原 VMA 保留前半(改 end), 新槽放后半(start=addr+len, offset 同步调整); 无空槽则失败处理(或先检查)
4. VMA 完全消失时: fileclose(v->f), v->used=0
- 考点8: munmap 后访问 → fault → VMA 找不到 → return 0 → trap.c 杀 ✓

## 5. 生命周期联动

- **fork**: kfork/allocproc 后复制父 VMA 数组(每个 filedup) + 物理页由 uvmcopy 常规拷贝(mmap 分支无 COW, 直接拷) ✓
- **exit**: 遍历 VMA: 脏页回写(SHARED) + uvmunmap 全部 + fileclose; 然后再走原 exit 流程
- **exec**: 放弃所有 VMA(fileclose), 清数组——地址空间本来就被换掉
- **close(fd)**: 只是 f->ref--(fileclose); mmap 时 filedup 过, 映射仍持一份引用 → 考点4 ✓
- **unlink**: 目录项删了但 file refcount>0 → inode 活着 → fault 仍能 readi ✓ 考点6/7

## 6. 文件引用计数关键（file.c 已有机制）

struct file 有 ref 字段; filedup() ref++; fileclose() ref-- 到 0 且 f->type==FD_INODE 时 iput。
mmap 登记时 filedup(f), VMA 消亡时 fileclose(f)。**这是"close(fd) 后映射仍有效"+"unlink 后仍可读"的唯一机制**。

## 7. 接线清单

kernel/syscall.h(+22,23) / syscall.c(extern+表) / defs.h(sys_mmap,sys_munmap 声明放 sysfile.c 实现) / user/usys.pl(entry×2) / user/user.h(void* mmap(void*,int,int,int,int,int); int munmap(void*,int)) / proc.h(vma struct+数组) / proc.c(allocproc/freeproc 清零, exit/exec/fork 联动) / vm.c(vmfault 扩展) / sysfile.c(sys_mmap/sys_munmap)

## 8. 坑预警

- mmap 区域别和 sbrk 增长区(p->sz 向上)相撞 → 从高地址向下分配, 留足边距(TRAMPOLINE/TRAPFRAME 下方)
- len 非整页: 上取整; 尾页读文件不足 PGSIZE 部分补零
- offset 非 0 的情况测试没考但接口留着(测试全传 0)
- VMA 分裂时 offset 别忘同步: 后半段 offset += (addr+len - 原start)
- munmap 部分页时 SHARED 脏页只回写被 munmap 的页
- exit 顺序: 先回写+释放 VMA, 再关原有 fd 表(uvmfree 前)
- writei 回写要用 begin_op/end_op 包事务! (sys_munmap/exit 里写文件必须走事务, exit 里原本没有 begin_op——注意 exit_mmap 时机, 参考讲义: exit 中回写在关闭文件之前, 需要 begin_op)
- fault 里 readi 目的地址是内核物理页(kalloc 返回值直接当内核地址用, KERNBASE 直映射区 ✓)

## 9. 验收结果（完成 ✅）

- **mmaptest 11 项全过一次通过**：basic/private/read-only/read-write/dirty/not-mapped-unmap/lazy-access/two-files/fork/munmap-prevents-access/writes-to-read-only → "all tests succeeded"
- **grade-lab-mmap = 170/170**（mmaptest 全项 + usertests OK 62.4s + time）；commit `55fc6fd`
- 首跑 169/170 丢的 1 分又是 time.txt（mmap 分支独立无此文件，补占位 20）

## 10. 实战踩坑录（4 个，全部当场修复）

1. **dirty 回写越过 EOF 撑大文件**（唯一功能性 bug）：测试要求 munmap 回写后文件保持 6144B（1.5页），我整页 writei(PGSIZE) 把它撑到 8192 → "dirty read #2" 期望返回 PGSIZE/2 实返 PGSIZE。修：回写前 `n = min(PGSIZE, ip->size - fileoff)`，fileoff ≥ size 则跳过——**mmap 永不改变文件大小**
2. 2025 版 `argint/argaddr` 返回 **void**（非老版 int），不能判 `< 0`；argfd/argstr 仍返 int
3. riscv.h **没有 PTE_D/PTE_A**，要自己加（bit7/bit6，RISC-V spec）；qemu 硬件自动置 D 位，脏页跟踪可行
4. sysfile.c 原本就有 `#include "proc.h"`（我只看前 12 行就追加）→ 重复 include 爆 redefinition（proc.h 无 guard）——**加 include 前先 grep 全文件**
5. vm.c 用 struct file 字段需 `#include "sleeplock.h"+"file.h"`（proc.h 里 file 只是前置声明指针）
