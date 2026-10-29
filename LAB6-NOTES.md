# LAB 6 · lock —— 设计笔记（进行中：任务 1 完成 70/100）

> 分支 `lock` · 2026-10-24~26 · commit `5a0f766`（任务 1）
> ⚠️ 2025 版重大变化：**没有 thread lab**！分支序列 = util/syscall/pgtbl/traps/cow/**lock**/fs/mmap/net（9 个）。旧版 thread（uthread+philosopher）被砍，其精髓（手搓 swtch）已在 LAB5-NOTES §4 的 swtch_demo 里提前修完。

## 0. 任务构成（总分 100）

| 任务 | 分值 | 状态 |
|---|---|---|
| kalloctest test1-4（per-CPU 内存分配器） | 40 | ✅ tot=0 / 95 |
| usertests sbrkmuch | 10 | ✅ |
| rwlktest（读写锁） | 30 | ⬜ 待做（§8） |
| usertests 全量 | 19 | ✅ 88.4s |
| time.txt | 1 | ✅（占位 20） |

---

## 1. 任务 1 问题与目标

原版 kalloc/kfree 全局一把 `kmem.lock` 一条 freelist——3 个 CPU 排一个窗口，kalloctest 实测争抢 18820 次（`#test-and-set` = acquire 自旋圈数）。目标：per-CPU freelist + 各自锁（**锁名必须 `kmem` 开头**——spinlock.c:488 统计按 `strncmp(name,"kmem",4)` 过滤，kalloctest 靠它算 tot）。

判定阈值：test1 `n-m < 10`；test4 `n-m < (NCHILD4-1)*10000 = 30000`。test4 行为 = 多个孩子无限循环 `sbrk(+3万页)/sbrk(-3万页)` 的分配风暴。

## 2. 数据结构与常规路径

```c
struct { struct spinlock lock; struct run *freelist; } kmem[NCPU];  // 8 个钱箱

kinit: 循环 initlock(&kmem[c].lock, "kmem") → freerange (全给 boot CPU 的箱,
       其他 CPU 靠 steal 自动摊平, 无需分发代码)

kfree: memset junk → push_off() → c=cpuid() → acquire(kmem[c]) → 挂自己箱 → release → pop_off()
kalloc: push_off() → c=cpuid() → 本地弹出; 空则 steal(c) 再弹 → pop_off() → memset 5
```

**push_off 必须在 cpuid 之前**（讲义 hint）：读完 hartid 到拿锁之间若被时钟中断迁去别的 CPU，"c" 就过期了。push_off 关中断 = 读号牌和办事之间不许搬家。（cpuid 就是读 tp 寄存器，uservec 进内核时装的 hartid。）

## 3. steal 的四轮进化（本 lab 最 valuable 的部分）

| 版本 | 策略 | 结果 | 死因 |
|---|---|---|---|
| v1 | 偷一半（锁内数链长+走链劈开，O(n)） | test4 ❌ tot=130万 | **锁内时间=别人的痛苦**：持 victim 锁走 3 万节点，victim 的 kfree 全程自旋 |
| v2 | 偷整链（单锁 O(1) 摘走，锁外拎回家入库） | test1 💥 崩溃 | **在途真空**：链条不在任何箱子的几微秒里，victim kalloc 扫遍全空 → 返回 0 → sbrk=-1 → 孩子写 0x3 被杀（stval=0x3 = -1+4，反汇编 `sw s5,4(a0)` 实锤） |
| v3 | 偷 64 页批量（锁内 O(64)） | test1 ✅ test4 ❌ tot=17万 | **总争抢 ≈ 总消耗 × 锁窗口，与批量无关**：批量小→steal 频次高，两者相乘不变 |
| **v4** | **双锁 + 整链原子过户（锁内 O(1)）** | **全绿 tot=95**（官方示例 9046） | — |

```c
static void steal(int me){
  for(other = 0..NCPU-1, != me){
    l0 = 编号小的锁; l1 = 编号大的锁;      // ★ 全局锁序: 永远先小号
    acquire(l0); acquire(l1);              // 两箱同时冻结
    if(kmem[me].freelist == 0 && kmem[other].freelist != 0){
      kmem[me].freelist = kmem[other].freelist;  // O(1) 过户
      kmem[other].freelist = 0;                  // 旧账本清零(防双重分配!)
    }
    release(l1); release(l0);
    if(自家箱非空) return;                  // 自己锁下确认成功
  }
}
```

## 4. 双锁的两个"为什么"

**为什么要两把锁**：转移同时改两份共享数据。只锁 other → 摘下后放入前链条"在途"（v2 真空崩溃）；只锁 me → 拿自家锁改别人账本，锁错门，victim 的 kfree 同时在挂链 → 断链/丢页。**双锁把"取出+放入"焊成原子操作：任何瞬间链条都登记在某个箱子名下**。

**为什么按编号排序拿锁**：CPU0 偷 CPU1 与 CPU1 偷 CPU0 并发时，若各先拿自己的再等对方的 → 各持一把互等 → 死锁（自旋锁还不能睡眠让路）。统一"先小号后大号" → 两个偷锁者抢同一把小锁，输家等赢家全做完 → 无环无死锁。**银行转账法则**（A→B 与 B→A 都先锁账号小的）。与 cow lab 的 `cow_lock→kmem.lock` 单向锁序同宗：双向操作靠排序化归成单向。

**`kmem[other].freelist = 0` 不能漏**：漏掉 = 同一条链挂在两个箱子上 → 两边各自弹出同一批物理页 → 双重分配 → 两个进程共享一张页互写 → 随机损坏极难复现。

## 5. 平衡哲学：纯 pull，无管理者

内存流动只有两条通道：kfree 回流（谁释放进谁的箱）+ steal 虹吸（谁缺谁去抢）。**没有**全局平衡器/周期摊平/归还机制。victim 被偷空后自食其力：等 kfree 回流，或下次 kalloc 反偷（O(1) 乒乓无害）。内存自动流向消费最旺的 CPU = 按需分配。test4 的 tot=95 正来自"全偷让 steal 频率降到每 3 万页一次"。

## 6. 真实世界对照（glibc 之问的答案）

楼层关系：app malloc → **glibc**(用户态二级批发) → brk/mmap → **内核 buddy+pcp**(物理页, kalloc 的真对标)。

| 系统 | 本地缓存 | 缺货找谁 | 偷同伴? |
|---|---|---|---|
| xv6 kalloc(本实现) | per-CPU freelist | **横向偷 CPU 同伴**(双锁过户) | ✅ |
| Linux 内核 | per-CPU pageset(pcp) | **纵向找 buddy 大仓库**(批拉); pcp 满了 drain 回去 | ❌ |
| glibc ptmalloc2 | **tcache**(每线程 64 bins, 完全无锁) + arena(≤8×核数) | tcache→arena(加锁)→brk/mmap | ❌ 靠多 arena 分流 |
| tcmalloc/jemalloc | thread cache(无锁) | central freelist 批转移 | jemalloc 有跨线程回收 |

共同灵魂：**本地小仓库优先 + 匮乏驱动拉取(pull) + 批量转移摊薄成本**。xv6 选"偷"因为没有中央仓库概念；Linux 选"仓库"因为 buddy 要管物理连续大块(DMA/hugepage)必须全局视图。tcache 无锁的原因 = thread-local 无共享则无竞态；arena 共享所以要锁。glibc 单 arena 时代 → ptmalloc 多 arena → 2.26 加 tcache，与 xv6 单锁 kmem → per-CPU 是三代人解同一道题。

## 7. 坑速查表（任务 1）

| # | 坑 | 拆法 |
|---|---|---|
| 1 | 锁内走长链 → victim 自旋爆表 | 锁内只许 O(1)（v1/v3 教训：总争抢≈总消耗×锁窗口） |
| 2 | 单锁全偷 → 在途真空 → 分配误判失败 | 双锁原子过户，链条永有所属 |
| 3 | 过户不清零旧账本 → 双重分配 | `other.freelist = 0` 是过户的另一半 |
| 4 | 双锁无序 → 互偷死锁 | 全局锁序：编号小者先拿 |
| 5 | cpuid 与用锁之间被迁移 | push_off/pop_off 包住（且所有 return 路径过 pop_off） |
| 6 | 锁名不含 kmem 前缀 → 统计不到 → 判分失败 | initlock 全叫 "kmem" |
| 7 | test1 孩子不检查 sbrk 返回值（写 a+4）| sbrk=-1 时写 0x3 被杀——内核侧分配失败会以这种诡异姿势暴露，排障时先反汇编崩溃点 |

## 8. 任务 2 · rwspinlock（30 分，rwlktest 30 步全过 4/4，待 grade 终验）

### 8.1 语义（阅览室规则）
多读并发 / 写独占（写时无读无其它写）/ **写者优先**：写者一登记，新读者靠边站，只等存量读者走完——否则读者源源不断，写者饿死。

### 8.2 毛坯现状（已侦察）
- `spinlock.h`: `struct rwspinlock { struct spinlock l; }` 占位，字段随我设计
- `spinlock.c`: 四个 `_inner` stub（现在错误地直接 acquire/release 占位小锁，多读者会 panic: acquire）；外壳 read/write_acquire/release 已包好 push_off/pop_off；initrwlock 待填
- 测试：`sys_rwlktest`（内核内 7 step 渐进）+ 用户程序 `rwlktest`；usertests 回归

### 8.3 方案 A（推荐）：内部小互斥锁罩住全部状态
```c
struct rwspinlock {
  struct spinlock l;    // 只保护下面三个计数, 临界区 O(1) 极短
  int readers;          // 正在读的个数
  int writing;          // 1 = 写者正在写
  int waiting_writers;  // 排队的写者数(>0 = "暂停营业"牌已挂)
};

read_acquire_inner:  循环{ acquire(l);
                       if(!writing && !waiting_writers){ readers++; release(l); return; }
                       release(l); /* 锁外自旋再试 */ }
read_release_inner:  acquire(l); readers--; release(l);
write_acquire_inner: acquire(l); waiting_writers++;
                     while(writing || readers>0){ release(l); 自旋; acquire(l); }
                     waiting_writers--; writing=1; release(l);
write_release_inner: acquire(l); writing=0; release(l);
```
**为什么这样 check-then-act 天然原子**：读者"看牌(!writing && !waiting_writers)"和"进门登记(readers++)"在**同一次持锁**里完成，写者挂牌/进场也在持锁里——讲义 hint 那个"看到无写者到进门之间写者插队"的竞态被小锁窗口整体罩住，与 cowfault 锁内 double-check、steal 双锁冻结同宗。
**不自旋持锁**：所有等待循环都是 release→自旋→acquire，不会持锁睡觉/死等。
**评分逻辑**：rwlktest 考功能正确性（非争抢数），小锁窗口 O(1) 完全够用，正确性肉眼可证——比无锁版稳。

### 8.4 方案 B（备选）：gcc __atomic 无锁版
字段同上（uint32）。读者：见无 writing/waiting_writers → `__atomic_add_fetch(readers,1)` → **复查**，发现写者出现则 `sub_fetch` 退回重试（增了再撤，写者见 readers 回落即进展，无死锁但有瞬时互等）。写者：先 `add_fetch(waiting_writers,1)` 挂牌 → 自旋等 readers==0 && writing==0 → CAS writing 0→1 → 摘牌。写者间靠 CAS 决胜负。难点在内存序（ACQUIRE/RELEASE/ACQ_REL 配对）与退回协议的正确性论证——除非方案 A 遇到性能测试，否则不用。

### 8.5 侦察结果（已验证，蓝图假设全部成立）
1. **并发模型**：用户 rwlktest fork **4 个进程**各调 syscall；屏障 `rwspinlock_test_step` 硬编码 `ncpu=4` 等四方到齐 → **必须 CPUS=4 跑**！CPUS=3 时前 3 进程在屏障上 push_off 关中断自旋等第 4 人，第 4 人永远无 CPU 可跑（关中断连时钟抢占都没）→ 第一步就死锁，一个横幅都不打（实测踩中，15 分钟无输出）
2. 等待双方永远在不同 hart → 关中断自旋安全，方案 A 无需 yield
3. 实现 = 方案 A 原样落地，零改动；initrwlock 三计数清零 + initlock
4. 附带发现：`-DSOL_LOCK`（Makefile:95）是全仓无人使用的死宏，不会短路自己的实现；工具链在 `tools/xpack-riscv-none-elf-gcc-14.2.0-3/bin`，新 shell 必须先 export PATH（否则退化到 clang 报 -mcmodel 错）

### 8.6 实测：共 **30 步**（讲义说 7 步，又一次讲义与实测不符）
step1-3 init/百万连发读锁并发；step4-5 写者优先（CPU1 拿 30 读锁、CPU0 写者挂牌、CPU2 新读者被拦，flag 验证不抢跑）；step6-7 百万轮读写混合互斥（共享计数器 v 验证写者在场 v≤1）；step8-9 **多锁同持**；step10-29 **多轮 multiple writer priority**（多写者排队，先到先得不饿死）；step30 done。结果：`rwspinlock_test(0..3): 0` + **`rwlktest: 4/4 CPUs succeeded`**，全程约 2.5 分钟。

### 8.7 验收（完成 ✅）
rwlktest 30 步 + 4/4 ✓（CPUS=4）→ **grade-lab-lock = 100/100**（kalloctest 五连 OK + rwlktest OK 14.4s + usertests OK 71.6s + time OK）→ commit `810b754`（spinlock.c/h，+31/-7 行）。

### 8.8 坑速查（任务 2）
- rwlktest 必须 **CPUS=4**（屏障硬编码 ncpu=4，少一个 CPU = 第一步死锁且无任何输出）
- expect 脚本里 `| tail` 会缓冲到进程结束——观测长测试要用 `log_file` 实时落盘
- 新 shell 跑 make 前先 `export PATH=<tools/xpack...>/bin:$PATH`
- execute_command 的 sleep 上限约 3-4 分钟，长等待用后台 + 分段 poll

## 9. 成果（截至目前）

```
5a0f766 lock lab task1: per-CPU kalloc freelists with O(1) atomic steal
Score: 70/100 (kalloctest 50 + usertests 19 + time 1; rwlktest 30 待做)
```

六连进度：util 131 → syscall 45 → pgtbl 41 → traps 95 → cow 130 → **lock 70/100(进行中)**
