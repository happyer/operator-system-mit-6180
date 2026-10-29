# LAB 9 · net 实现与验收

分支 `net`，base `982b43b`。2026-10-29 最终复验 **171/171**：9 项网络功能及 time 全通过。用户报告实际用时 **4 小时**，`time.txt=4`。已提交：`039fa90`。

## 0. 修正此前侦察结论

此前“只写两个函数、协议栈全现成”的说法错误：只搜索了部分函数，未完整阅读 net.c。真实任务分两部分：

1. kernel/e1000.c：e1000_transmit、e1000_recv。
2. kernel/net.c：sys_bind、sys_recv、ip_rx；另外实现可选 sys_unbind，并修复发送失败回收。

2025 版使用 kalloc/kfree 裸页，没有 mbuf。系统调用已接线不代表实现已经完成。nettest 的 ping0–3 是 UDP 回音测试，不是 ICMP。

## 1. E1000 发送

- 增加静态 tx_bufs[16]，记录驱动拥有的数据页。
- 读取 TDT，检查对应描述符 DD 位；未完成则返回 -1，调用者负责释放新 buf。
- DD 表示上一轮已处理完成，屏障后释放该槽旧页。
- 写 addr/length、EOP|RS 命令，清 status，保存新页指针。
- 屏障后更新 TDT=(tail+1)%16，最后释放驱动锁。
- TX 描述符位于普通 RAM，TDT/TDH 位于 MMIO 寄存器区域；DMA 地址使用 xv6 恒等映射内核指针数值。

## 2. E1000 接收

- 持锁读取下一个待收割槽 idx=(RDT+1)%16，检查 DD。
- 屏障后读取长度及状态，要求 EOP、errors=0、0<len<=2048。
- 保存旧 buf，分配新页并替换 addr，清 status，屏障后将 RDT 更新为 idx。
- 放锁后调用 net_rx；它可能发送 ARP 回复，再次获取驱动锁，不能持锁调用。
- 若补页失败：保留原页供硬件复用，丢弃当前包，不 panic、不把仍归硬件的页交给协议栈。
- 非法包释放旧页。一轮中断循环处理所有已经完成的槽。
- RX 硬件使用 RDH 指向的可用槽，推进 RDH；RDT 为软件补充缓冲区的边界。不要理解为“硬件从 RDT+1 收包”：RDT+1 是本实现的软件收割位置。

## 3. UDP 端口队列

- 静态最多 32 个端口，每端口最多 16 个包，netlock 保护。
- 每包保存 buf、payload offset/len、主机字节序 src IP/sport。
- bind：验证端口范围，拒绝重复绑定，建立空队列；绑定全局可由 fork 后子进程使用。
- recv：读取参数，查绑定；空队列用 sleep(q,&netlock) 原子等待；检查 killed、unbind 与槽复用 generation。
- 取出队首后释放 netlock，再 copyout 来源 IP、来源端口及 min(payload长度,maxlen) 数据；任何失败仍释放数据页。
- unbind：释放全部积压页、取消绑定、唤醒等待者；generation 防止旧等待者误消费新绑定。
- ip_rx：保留评分需要的 printf；验证 IPv4 版本、头长度、IP/UDP 长度、协议、目的 IP，拒绝分片；支持按 IHL 定位 UDP。
- 未绑定/队列满/非法包直接 kfree；有效包按目标端口入 FIFO，wakeup。
- sys_send/arp_rx：e1000_transmit 失败时 kfree；sys_send 返回 -1，增加负长度/端口范围校验。

## 4. 验收记录

- 编译：make LAB=net kernel/kernel fs.img，通过 -Wall -Werror。
- 两个修改源码的 IDE diagnostics：无错误。
- 官方 grade-lab-net：16.2s，txone / arp_rx / ip_rx / ping0 / ping1 / ping2 / ping3 / dns / free 全 OK。
- DNS 实际回复：pdos.csail.mit.edu → 128.52.129.126。
- 首跑170/171，仅time.txt缺失；用户提供真实用时4小时后填写4，最终复验15.2s，**171/171**。
- 最终日志 /tmp/net-final-grade.log；首跑日志 /tmp/net-grade.log；内核控制台 xv6-labs-2025/xv6.out。
- 提交 `039fa90`：仅 kernel/e1000.c、kernel/net.c、time.txt；未改用户测试及评分脚本，未处理原有未跟踪 fs.img.bk。
- 根目录 README.md、STUDY-PLAN.md 已更新，九个Lab累计983/983（各分支历史验收记录合计）。

## 5. 本次教训

- 初始搜索漏了大写 `Your code here`，导致错误宣称协议栈完成；实现前必须完整阅读任务相关文件。
- 不能拿“测试不会填满环”作为忽略失败释放的理由；实际 ping3 连续发送 257 个包，所有权失败路径必须闭合。
- 同 CPU 持锁关中断仅避免本核中断重入；跨核仍靠锁串行化，不能因此断言所有嵌套调用安全。
- net_rx 回调可再次发送，驱动锁必须提前放开。
