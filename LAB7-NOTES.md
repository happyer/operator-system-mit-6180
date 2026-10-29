# LAB 7 · fs 设计笔记（2025 版）

> 分支 `fs`，base `a95dec4`。两任务：large files（bmap 双间接）+ symlinks。
> 状态：bigfile ✓ / symlinktest ✓ / usertests ✓ / grade 终验跑中（结果见 §7）

## 1. 侦察结论（动工前实锤）

- **FSSIZE 条件编译**（kernel/param.h:17）：`LAB_FS` 时 = **200000 块 ≈ 200MB**（lock 分支只有 10000，riscv 基线 2000）——不确认这个，bigfile 写到一半 balloc 返回 0
- **bigfile 判据**：必须恰好写出 **65803 块**（= 11 + 256 + 256²）再逐块读回校验，多一块少一块都 FAIL
- **槽位陷阱**：baseline `addrs[NDIRECT+1]` = 12直+1单间 = 13 槽；把 NDIRECT 改 11 后若不动数组声明只剩 12 槽，**双间接总册没地方放**——必须同步改 `addrs[NDIRECT+2]`，且**磁盘版 dinode（fs.h）与内存版 struct inode（file.h:29）两处都要改**；ilock/iupdate 用 `sizeof(ip->addrs)` 拷贝，自动适配
- dinode 总大小不变（8+4+13×4=64B），磁盘布局兼容，mkfs 无需动
- **UPROGS**：fs 段只有 `_bigfile`，`_symlinktest` 要自己加（Makefile:264，tab 缩进，`cat -et` 验证）
- symlinktest 考点（代码实锤）：创建/stat 类型/追踪读/目标删除后 open 失败/**循环链 b→a→b 必须失败**/目标不存在创建仍成功/3 层深链 1→2→3→4/**NINODE+2 个海量 symlink**（专考 iput 纪律）/concur 并发/O_NOFOLLOW

## 2. 任务 1 · large files（3 处改动）

### fs.h
```c
#define NDIRECT 11                          // 12→11
#define NINDIRECT (BSIZE / sizeof(uint))    // 256
#define NDOUBLY (NINDIRECT * NINDIRECT)     // 65536 新增
#define MAXFILE (NDIRECT + NINDIRECT + NDOUBLY)   // 65803
uint addrs[NDIRECT+2];   // 11直 + 1单间(addrs[11]) + 1双间总册(addrs[12])
```
（file.h 内存版 inode 同步改 `addrs[NDIRECT+2]`）

### bmap() 三段式翻译
```
bn < 11        → 直接槽 addrs[bn]（无则 balloc）
bn < 11+256    → 单间接: addrs[11] 索引册, a[bn]（原逻辑平移）
bn < 65803     → 双间接: addrs[12] 总册 → a[bn/256] 定位子册(无则 balloc+log_write 总册)
                        → 子册 a2[bn%256] 定位数据块(无则 balloc+log_write 子册)
                        → brelse(bp2); brelse(bp)   ← 两层 bread 严格配对
```
纪律：所有写块一律 `log_write` 不 `bwrite`；balloc 失败路径也要先 brelse 再 return 0。
事务余量核算：单次双间接 ≈ bitmap1+总册1+子册1+数据块1+inode1 = 5 块 < MAXOPBLOCKS(10) ✓

### itrunc() 三层释放
直接 11 块 bfree → 单间接：遍历 256 条 bfree + 册本身 bfree → **双间接：遍历总册 256 本，每本翻开 bfree 其 256 数据块 + bfree 子册本身，最后 bfree 总册**。漏任何一层 = bitmap 泄漏，磁盘越用越满。

## 3. 任务 2 · symlinks（8 文件）

| 文件 | 改动 |
|---|---|
| kernel/stat.h | `#define T_SYMLINK 4` |
| kernel/fcntl.h | `#define O_NOFOLLOW 0x800` |
| kernel/syscall.h | `#define SYS_symlink 22` |
| kernel/syscall.c | extern + 表项（五件套之二） |
| user/user.h | `int symlink(const char*, const char*);` |
| user/usys.pl | `entry("symlink");`（2025 版是 entry() 不是 SYSCALL()） |
| Makefile | UPROGS 加 `$U/_symlinktest` |
| kernel/sysfile.c | **两处真逻辑**（见下） |

### sys_symlink（便利贴 = 内容为路径字符串的普通小文件）
```
argstr(0,target) argstr(1,path)      ← 参数序同 Linux: symlink(target, linkpath)
begin_op → create(path, T_SYMLINK,0,0) → writei(ip,0,target,0,strlen) → iunlockput → end_op
```
- create 对已存在名字返回 0（T_SYMLINK 不走 T_FILE 复用分支）→ 重名自动失败 ✓
- **目标不存在也创建成功**（便利贴只是存了字符串）——与 hard link 的本质区别
- writei 内部自动更新 size + iupdate

### sys_open 追踪循环（else 分支 ilock 之后）
```c
int depth = 0;
while(ip->type == T_SYMLINK && !(omode & O_NOFOLLOW)){
  if(++depth > 5) { iunlockput; end_op; return -1; }   // 循环链保险丝
  memset(target,0); sz=min(ip->size, MAXPATH-1);
  readi(ip,0,target,0,sz);          // 读出目标路径
  iunlockput(ip);                   // ★ 每层必须归还(海量 symlink 测试专考这个)
  ip = namei(target);               // 目标被删 → namei 返回 0 → open 失败 ✓
  ilock(ip);
}
```
- 深度上限 5：够 1→2→3→4（3 跳），循环 b→a→b 撞限失败 ✓
- iunlockput 纪律：inode 缓存只有 NINODE(50) 个坑，NINODE+2 个 symlink 逐个 open，漏放一层即枯竭
- O_NOFOLLOW：不进循环，fd 指向便利贴本身 → fstat 得 T_SYMLINK ✓
- T_SYMLINK 的 unlink/stat/itrunc 全走普通文件路径，零额外改动

## 4. 用到的知识（缺口清单回填）

bread/brelse 配对（工作台借还）、log_write≠bwrite（断电流水账）、balloc/bfree（bitmap 集装箱调度）、ilock/iunlock/iref/iput（复印件借还四动词）、readi/writei（按 inode 读写内容）、create() 流水线（抄 sys_open 模板）。黑盒不深究：log 提交机制、virtio 驱动、dirlookup/dirlink 内部。

## 5. 验证记录

| 测试 | 结果 |
|---|---|
| bigfile（任务1改完立即跑） | **65803 块写入+读回, "bigfile done; ok"** 一次通过 |
| usertests -q（itrunc 回归） | **ALL TESTS PASSED** 一次通过 |
| symlinktest | **test symlinks: ok + test concurrent symlinks: ok** 一次通过 |
| grade-lab-fs | 跑中 → §7 回填 |

## 6. 坑速查

- NDIRECT 12→11 时**两处** addrs 声明（fs.h dinode + file.h inode）都要 +2，漏一处 = 越界写内存
- Makefile UPROGS 行尾 `\` 续行、tab 缩进（cat -et 验证，macOS cat 无 -A）
- usys.pl 2025 版格式是 `entry("name");`
- bmap 双间接的 balloc 失败路径必须先 brelse 已 bread 的册子再 return
- itrunc 忘记释放子册/总册本身 → bitmap 泄漏，usertests 的 disk 相关测试会炸
- 追踪循环每层 iunlockput——NINODE+2 海量测试就是冲这个来的
- 跑测试：expect + log_file 实时落盘（tail 管道会缓冲到进程结束）；工具链 PATH=tools/xpack-.../bin

## 7. 终验与提交

- **grade-lab-fs = 100/100**（bigfile OK + symlinktest 双 OK + usertests OK 185.9s + time OK）
- commits：任务1 `0d17035`（fs.h/fs.c/file.h），任务2 `094bb85`（接线7件+sysfile.c+time.txt）
- 插曲：首跑 99/100，丢的 1 分是 time.txt 被误删（补回占位 20）
- 全程零返工：bigfile/symlinktest/usertests 均一次通过
