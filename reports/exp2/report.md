# 实验2: 系统调用修复与分析

## 2.1 概述

24 个 PR，覆盖信号处理、进程凭证、VFS、内存管理、epoll、IPC、网络、定时器等子系统。每个 PR 解决一个定义明确的问题，附带回归测试，可独立审查和合入。它们构成了 StarryOS 从基本可用到语义正确的关键一步。

按子系统分布:

| 子系统 | PR 数 | 代表性问题 |
|--------|-------|-----------|
| Signal | 3 | SA_RESTART 语义, sigaltstack 边界, epoll_pwait sigsetsize |
| Credentials | 1 | 完整凭证子系统实现 |
| VFS / FS | 5 | rename 原子性, fsync 目录, copy_file_range, ftruncate/pwrite64, sync_file_range |
| Memory | 2 | mremap 实现, RLIMIT_STACK 默认值 |
| epoll | 3 | CTL_MOD 事件丢失, LT 重复事件, sigsetsize |
| IPC | 1 | SMP 共享内存死锁 |
| Network | 4 | AF_UNIX EOF, socket chown, preadv/pwritev ABI, getrandom |
| Timer | 1 | timerfd 实现 |
| Task/Prctl | 3 | PDEATHSIG, prlimit64, interrupt waker |
| Arch/Platform | 2 | loongarch64 smoke, user crash dump |

## 2.2 PR 汇总

| PR # | 日期 | 标题 | 类型 |
|------|------|------|------|
| [#197](https://github.com/rcore-os/tgoskits/pull/197) | 04-15 | fix preadv/pwritev/preadv2/pwritev2 syscall ABI | Fix |
| [#205](https://github.com/rcore-os/tgoskits/pull/205) | 05-07 | feat: implement mremap | Feature |
| [#207](https://github.com/rcore-os/tgoskits/pull/207) | 04-25 | fix: accept sigaltstack with exactly MINSIGSTKSZ | Fix |
| [#208](https://github.com/rcore-os/tgoskits/pull/208) | 04-19 | fix getgroups size=0 and clock_gettime invalid clock_id | Fix |
| [#209](https://github.com/rcore-os/tgoskits/pull/209) | 04-30 | fix: negative value checks to ftruncate and pwrite64 | Fix |
| [#210](https://github.com/rcore-os/tgoskits/pull/210) | 04-23 | fix: validate getrandom flags per Linux semantics | Fix |
| [#211](https://github.com/rcore-os/tgoskits/pull/211) | 04-24 | fix: validate copy_file_range flags, types, and overlap | Fix |
| [#226](https://github.com/rcore-os/tgoskits/pull/226) | 04-21 | fix: resolve SHM AB/BA deadlock under SMP | Fix |
| [#246](https://github.com/rcore-os/tgoskits/pull/246) | 04-18 | feat: per-process credentials subsystem | Feature |
| [#247](https://github.com/rcore-os/tgoskits/pull/247) | 04-18 | feat: implement SA_RESTART semantics | Feature |
| [#248](https://github.com/rcore-os/tgoskits/pull/248) | 04-18 | feat: fix RLIMIT_STACK default to 8 MiB | Feature |
| [#249](https://github.com/rcore-os/tgoskits/pull/249) | 04-18 | feat: PR_SET_PDEATHSIG/PR_GET_PDEATHSIG | Feature |
| [#250](https://github.com/rcore-os/tgoskits/pull/250) | 04-18 | fix: epoll_pwait sigsetsize with musl | Fix |
| [#251](https://github.com/rcore-os/tgoskits/pull/251) | 04-18 | fix: fsync/fdatasync on directory fds + sync_file_range | Fix |
| [#311](https://github.com/rcore-os/tgoskits/pull/311) | 04-24 | fix: return EOF on unix stream recv when peer dropped | Fix |
| [#312](https://github.com/rcore-os/tgoskits/pull/312) | 04-24 | fix: preserve source DirEntry across rename | Fix |
| [#313](https://github.com/rcore-os/tgoskits/pull/313) | 04-30 | fix: chown unix socket file to binding credentials | Fix |
| [#314](https://github.com/rcore-os/tgoskits/pull/314) | 04-24 | fix: re-queue interest after EPOLL_CTL_MOD | Fix |
| [#316](https://github.com/rcore-os/tgoskits/pull/316) | 04-24 | fix: register interrupt waker before flag swap | Fix |
| [#318](https://github.com/rcore-os/tgoskits/pull/318) | 04-23 | fix: align loongarch64 smoke QEMU memory | Fix |
| [#319](https://github.com/rcore-os/tgoskits/pull/319) | 04-24 | fix: prlimit64 allow raising hard limit | Fix |
| [#503](https://github.com/rcore-os/tgoskits/pull/503) | 05-16 | feat: timerfd_create/settime/gettime | Feature |
| [#504](https://github.com/rcore-os/tgoskits/pull/504) | 05-12 | fix: drain ready list once per epoll_wait in LT mode | Fix |
| [#505](https://github.com/rcore-os/tgoskits/pull/505) | 05-16 | feat: dump user register state on fatal signals | Feature |

## 2.3 重点 PR 分析

### 2.3.1 SA_RESTART (#247)

设置了 `SA_RESTART` 的信号处理函数返回后，被中断的阻塞系统调用仍然返回 `EINTR`。这个 bug 涉及三个正交的内核概念。

信号处理与系统调用的交互。进程在阻塞系统调用（比如 `read`）中收到信号时，内核需要: 保存当前系统调用上下文，切换到用户态执行信号处理函数，处理函数返回后根据 `SA_RESTART` 标志决定是重启系统调用还是返回 `EINTR`。

架构相关的寄存器复用。这是这个 PR 最微妙的部分。RISC-V 的 `a0`、AArch64 的 `x0`、LoongArch64 的 `a0` 寄存器既是系统调用第一参数寄存器，又是返回值寄存器。内核把 `EINTR`（一个负数 errno）写进 `a0` 后，原始的 `a0`（比如 `fd`）就丢了。重启系统调用必须: 在进入 `handle_syscall` 前保存原始 `a0`，重启时 PC 回退 `SYSCALL_INSN_LEN` 字节（RISC-V/AArch64: 4, x86_64: 2），恢复 `a0` 为原始参数值，并且不能调 `set_retval(0)` 因为在 RISC-V/AArch64 上这会覆盖刚恢复的 `a0`。

x86_64 上 `rdi`（arg0）和 `rax`（retval）是不同的寄存器，不存在这个冲突。但实现必须对所有架构一致。

修复引入 `SyscallRestartInfo { saved_a0: usize }`，信号处理路径检测到 `SA_RESTART` 且当前返回值是 `EINTR` 时，PC 回退并恢复 `a0`。`rt_sigtimedwait` 和 `rt_sigsuspend` 这些本就期望 `EINTR` 语义的调用点传入 `None`。

这个 PR 是理解操作系统如何管理用户态上下文的典型案例。信号处理涉及用户态和内核态的多次切换、寄存器状态的保存恢复、以及架构 ABI 的精确知识。

### 2.3.2 进程凭证子系统 (#246)

StarryOS 的 `getuid`/`geteuid` 始终返回 0，权限检查缺失，IPC 子系统中 uid/gid 固定为 0。这个 PR 实现了完整的 `struct cred` 模型。

凭证模型包含 9 个 ID 字段: `uid`/`gid`（真实身份）、`euid`/`egid`（有效身份，用于大多数权限检查）、`suid`/`sgid`（保存的 set-user-ID，允许进程在特权和非特权身份间切换）、`fsuid`/`fsgid`（文件系统访问专用，正常情况下与 euid/egid 相同但可独立修改）、补充组列表。

凭证通过 `Arc<Cred>` 引用计数共享。`fork` 时子进程继承父进程凭证。`execve` 时根据可执行文件的 setuid/setgid 位更新 euid/egid。不同操作使用不同的权限模型: 文件读写检查 `fsuid` 对 owner/group/other 位的匹配；`chown` 需要 `CAP_CHOWN`（fsuid==0）或文件属于调用者且目标 gid 在补充组内；`kill` 要求发送者的 `{euid, uid}` 匹配目标的 `{uid, euid, suid}` 之一；资源限制提高需要 `CAP_SYS_RESOURCE`。

这个 PR 的基础性在于: 没有它，PostgreSQL 的 `initdb` 在创建数据目录时对 `chown`/`chmod` 的所有调用要么失败要么静默无效，后续所有基于 postgres 用户的操作全部错误。

### 2.3.3 epoll LT 模式就绪队列 (#504)

LT（电平触发）模式下 `epoll_wait(maxevents=N)` 对单个就绪 fd 返回 N 份相同的 `epoll_event`。Weston、libuv、Go netpoll 等基于 `epoll_wait(maxevents>1)` 的事件循环全受影响。

根因在 `Epoll::poll_events` 的循环里: `pop_front` 取出 `interest` 写进 `out[count]`，然后同一个 `Weak<EpollInterest>` 被 `push_back` 回 `ready_queue`。下一轮 `pop_front` 拿到的还是同一个 interest。一次就绪被报告 `maxevents` 次。

Linux `fs/eventpoll.c` 的 `ep_send_events` 用的是完全不同的模式: drain txlist → iterate once → requeue LT survivors。修复对齐了这个模式: `mem::take` 一次性排空 `ready_queue` 到本地 `txlist`，每个 interest 只访问一次，LT 留下的放 `keep` 队列，循环结束整体 splice 回去并 `wake()`。

还有一个并发问题。`consume()` 检查和 `register()` 之间的窗口会导致永久丢失事件: 新事件如果在 `consume()` 返回 false 之后、`register()` 之前到达，waker 看到 `in_ready_queue=true` 直接放弃 wake。修复采用 `check_and_register_waker` 模式，注册 waker 后再读一次 `file.poll()`，匹配新事件就立刻 wake。

这个 PR 是并发数据结构正确性的经典案例。一个简单的 `push_back` 放到循环里就能把 O(1) 语义变成 O(N) 的重复。同时涉及 wait-free 消费者和 lock-free 生产者的协调。

### 2.3.4 VFS rename 原子性 (#312)

tmpfs 上 `rename(src, dst)` 后 `dst` 内容变成全零。PostgreSQL 的 `durable_rename` 流程: 写临时文件 → `fsync` → `rename` 到最终路径。rename 后 page cache 丢失，`pg_control` 读成全零，启动报 "replication checkpoint has wrong magic 0"。

tmpfs 没有持久存储，文件内容只存在于 DirEntry 的 `user_data` 字段通过 `Arc` 持有的 page cache 中。`DirNode::rename` 对源路径调 `forget_entry`，把 DirEntry 从目录 cache 移除，`Arc` 引用计数归零，page cache 释放。后续 lookup 目标文件名拿到全新空 page cache。

`rename` 的语义是把同一个文件对象从旧名称重关联到新名称，不是复制内容然后删旧路径。修复将源 DirEntry 从源目录取出（而非 forget），以目标文件名插入目标目录，保持 DirEntry 实例不变。目标路径的旧 entry 仍然 forget，因为它已被替换。

### 2.3.5 IPC 共享内存死锁 (#226)

SMP 环境下 `sys_shmget` 和 `sys_shmat` 存在 AB/BA 死锁。SMP=4 压力测试复现率 95%，20 次中 19 次超时。

`sys_shmget` 的加锁顺序是 `SHM_MANAGER → shm_inner`，`sys_shmat` 是 `shm_inner → aspace → SHM_MANAGER`。经典死锁: 线程 1 持有 A 等 B，线程 2 持有 B 等 A。

解决方案不是加更大的锁。`sys_shmdt` 的修复采用分阶段加锁: 查 shmid（持 SHM_MANAGER 后释放）→ 读 va_range（持 shm_inner 后释放）→ 解除映射（仅持 aspace）→ 更新 bookkeeping（重新获取 SHM_MANAGER 和 shm_inner）。全局锁顺序统一为 `SHM_MANAGER → shm_inner → aspace`，与 Linux IPC 子系统一致。

修改锁顺序时必须检查传递性: `aspace.map()` 内部是否会回锁 SHM 相关状态？逐层检查了 `AddrSpace → MemorySet → SharedBackend` 的调用链确认没有循环依赖。同时修复了 `clear_proc_shm` 缺少 `aspace.unmap()` 导致的 use-after-free。

锁顺序问题不会在单核上暴露，只有 SMP 压力测试才能稳定复现。修复锁顺序的同时往往需要重构代码结构（分阶段加锁），不是简单交换两行 `lock()` 就能解决。

## 2.4 共性结论

四类典型缺陷。

晚期标志检查。`sys_mmap` 先分配内存再检查 `MAP_ANONYMOUS`，`copy_file_range` 先读数据再检查 flags。正确的模式是在入口处立即校验所有参数，fail-fast。

遗留与新实现的混淆。`preadv`/`pwritev` 的 64 位偏移分成 `pos_l` 和 `pos_h` 两个参数，实现只读了 `pos_l`。这是从 32 位过渡到 64 位时的常见遗漏。

直觉驱动的边界处理。`<= MINSIGSTKSZ` 拒绝恰好等于最小值的大小，epoll sigsetsize 严格等于 8 拒绝了 musl 的 16 字节。Linux 的边界处理有精确的原因（man page 说明、历史兼容性），不是直觉能替代的。

错误路径的副作用。`prlimit64` 不能提高限制时返回 Ok 而非 Err，`rename` 的 `forget_entry` 释放了不应释放的 page cache。最危险的是看起来成功了但什么都没做的错误处理。
