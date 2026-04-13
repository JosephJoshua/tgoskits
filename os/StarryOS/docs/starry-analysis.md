# StarryOS 深度源码分析报告

> 基于 StarryOS v0.4.0 workspace (commit eab9e0a8)，ArceOS 依赖版本 v0.5.0
> 2025全国大学生OS比赛内核赛道一等奖项目

---

## 目录

1. [架构概览](#1-架构概览)
2. [多核支持能力分析与优化方案](#2-多核支持能力分析与优化方案)
3. [最值得改进的10个点及改进计划](#3-最值得改进的10个点及改进计划)
4. [Linux Syscall支持能力与缺陷分析](#4-linux-syscall支持能力与缺陷分析)
5. [Syscall优先实现排序](#5-syscall优先实现排序)
6. [AI自动迭代编程方法设计](#6-ai自动迭代编程方法设计)
7. [面试准备：核心问题与回答要点](#7-面试准备核心问题与回答要点)
- [附录 A：210个 Sysno 变体完整清单](#附录-a210个-sysno-变体209个独立-syscall完整清单)
- [附录 B：源码中的 TODO/FIXME 统计](#附录-b源码中的-todofixme-统计)
- [附录 C：自动化 Syscall 测试结果](#附录-c自动化-syscall-测试结果)

---

## 1. 架构概览

### 1.1 设计哲学

StarryOS 采用 **"ArceOS基座 + 宏内核扩展"** 的架构。ArceOS 是一个模块化的 unikernel 框架（library OS），StarryOS 在其之上构建了完整的宏内核功能层，实现 Linux syscall 兼容。

```
+---------------------------------------------+
|              User Applications              |
|         (busybox, redis, nginx...)          |
+---------------------------------------------+
|            Linux Syscall Interface          |  <-- kernel/src/syscall/ (210个syscall)
|  +------+------+------+------+----------+   |
|  |  fs  |  mm  | task | net  | signal   |   |
|  |      |      |      |      | ipc/sync |   |
|  +------+------+------+------+----------+   |
+---------------------------------------------+
|         Kernel Core (starry-kernel)         |
|  +----------+----------+----------------+   |
|  | Process  | VirtMem  |  PseudoFS      |   |
|  | (starry- | (starry- |  (/dev,/proc,  |   |
|  | process) |   vm)    |   /tmp,/sys)   |   |
|  +----------+----------+----------------+   |
+---------------------------------------------+
|          ArceOS Framework Layer             |
|  +--------+--------+--------+-----------+   |
|  |ax-task | ax-mm  | ax-fs  |  axnet    |   |
|  |(sched) |(page)  |(ext4)  | (smoltcp) |   |
|  +--------+--------+--------+-----------+   |
|  |ax-sync |ax-hal  |ax-drv  | ax-alloc  |   |
|  |(lock)  |(HAL)   |(virtio)| (slab)    |   |
|  +--------+--------+--------+-----------+   |
+---------------------------------------------+
|     Hardware (RISC-V / AArch64 / LA64)      |
+---------------------------------------------+
```

### 1.2 代码结构

```
StarryOS/
+-- starryos/src/main.rs      # 二进制入口，配置 CMDLINE 后调用 kernel
+-- kernel/src/
|   +-- entry.rs              # 内核初始化：挂载fs -> 加载ELF -> 创建init进程
|   +-- config/               # 各架构的地址空间布局 (RISC-V/AArch64/LA64/x86)
|   +-- syscall/              # 210个系统调用实现 (~7800行)
|   |   +-- mod.rs            # 中央分发表 (640行，其中 match 语句约606行)
|   |   +-- fs/               # 文件系统相关
|   |   +-- mm/               # 内存管理
|   |   +-- task/             # 进程/线程管理
|   |   +-- net/              # 网络
|   |   +-- signal.rs         # 信号
|   |   +-- io_mpx/           # IO多路复用 (select/poll/epoll)
|   |   +-- ipc/              # IPC (消息队列/共享内存)
|   |   +-- sync/             # 同步 (futex/membarrier)
|   |   +-- time.rs           # 时间
|   |   +-- sys.rs            # 系统信息
|   |   +-- resources.rs      # 资源限制
|   +-- task/                 # 任务管理核心
|   |   +-- mod.rs            # Thread/ProcessData 数据结构
|   |   +-- user.rs           # 用户态任务生命周期
|   |   +-- futex.rs          # Futex实现（等待队列/哈希表）
|   |   +-- signal.rs         # 信号投递逻辑
|   |   +-- timer.rs          # 定时器（ITIMER_REAL/VIRTUAL/PROF）
|   |   +-- resources.rs      # 资源限制结构
|   +-- mm/                   # 内存管理
|   |   +-- aspace/           # 地址空间管理
|   |   |   +-- mod.rs        # AddrSpace (页表 + MemorySet)
|   |   |   +-- backend/      # 映射后端 (Linear/CoW/Shared/File)
|   |   +-- loader.rs         # ELF加载器（支持动态链接）
|   |   +-- access.rs         # 用户态内存访问
|   +-- pseudofs/             # 伪文件系统
|   |   +-- dev/tty/          # TTY/PTY完整实现（行纪律/作业控制）
|   |   +-- proc.rs           # /proc文件系统
|   |   +-- tmp.rs            # tmpfs (/tmp, /dev/shm)
|   +-- file/                 # 文件抽象层
|       +-- epoll.rs          # epoll实现
|       +-- pipe.rs           # 管道 (64KB环形缓冲区)
|       +-- signalfd.rs       # signalfd
+-- make/                     # 构建系统 (Makefile)
```

### 1.3 支持的架构

| 架构 | 目标三元组 | 支持等级 | 页表模式 |
|------|-----------|---------|---------|
| RISC-V 64 | riscv64gc-unknown-none-elf | 主力 | 单 SATP 寄存器，内核映射需复制到用户页表 |
| AArch64 | aarch64-unknown-none-softfloat | 完善 | TTBR0/TTBR1 分离，无需复制 |
| LoongArch64 | loongarch64-unknown-none-softfloat | 完善 | PGDL/PGDH 分离，无需复制 |
| x86_64 | x86_64-unknown-none | WIP | 内核映射需复制到用户页表 |

### 1.4 启动流程

```
1. QEMU/硬件启动 -> ax-runtime (ArceOS)初始化
2. starryos/main.rs::main()
   +-- 设置 CMDLINE = ["/bin/sh", "-c", "init脚本"]
   +-- 调用 starry_kernel::entry::init(args, envs)
3. entry::init()
   +-- mount_all() -> 挂载 /dev, /proc, /tmp, /sys, /dev/shm
   +-- spawn_alarm_task() -> 启动定时器管理任务
   +-- 解析可执行文件路径，创建用户地址空间
   +-- load_user_app() -> ELF加载（支持动态链接/interpreter）
   +-- 创建 UserContext, Thread, ProcessData, Process
   +-- 绑定 N_TTY (终端)，设置 stdio (fd 0/1/2)
   +-- spawn_task() -> 提交到调度器
   +-- 等待进程退出，卸载文件系统
```

---

## 2. 多核支持能力分析与优化方案

### 2.1 当前多核支持现状

StarryOS 的多核支持采用 **"委托模型"**：SMP 底层由 ArceOS (ax-runtime, ax-task, ax-hal) 实现，StarryOS 内核层专注于提供多核安全的高层抽象。

#### 2.1.1 SMP启动

- 多核启动由 ax-runtime 和 ax-hal 完成（通过 smp feature 启用）
- StarryOS内核本身不含汇编级的副核启动代码
- 启动命令：make SMP=4 run 启用4核

#### 2.1.2 调度器

- 使用 ArceOS 的 sched-rr（Round-Robin）调度策略
- **每核独立运行队列**：通过 ax-task 的 `smp` feature 实现（`multitask` 启用调度器，`smp` 使其变为 per-CPU）
- **CPU亲和性**：支持 sched_setaffinity/sched_getaffinity，使用 AxCpuMask 位掩码
- **局限**：
  - 仅支持 SCHED_RR，不支持 SCHED_FIFO/SCHED_OTHER/SCHED_DEADLINE
  - 亲和性设置仅对当前线程生效（kernel/src/syscall/task/schedule.rs:96 有TODO）。注：ax-task 底层支持通过 `set_current_affinity` 触发跨核迁移（spawn migration task），但 StarryOS syscall 层限制 pid!=0 时返回 EPERM
  - 无工作窃取（work-stealing）：CPU 空闲时进入 idle，不从其他核偷取任务
  - 无运行时负载均衡：仅在 spawn 时通过原子计数器 round-robin 分配到各核（ax-task run_queue.rs:98-111），之后任务固定在该核上。ax-task 源码有 TODO 注释明确标注 load balancing 未实现（run_queue.rs:148-151）

#### 2.1.3 同步原语

| 原语 | 实现位置 | 说明 |
|------|---------|------|
| SpinNoIrq | ax_kspin | 关中断自旋锁，用于futex等待队列 |
| Mutex | ax_sync | 阻塞互斥锁，保护地址空间等 |
| RwLock | spin::RwLock | 读写锁，用于FD表 |
| Atomic* | core::sync::atomic | 广泛使用(58处 Ordering:: 引用)，SeqCst/Release/Acquire |
| Futex | kernel/src/task/futex.rs | 完整实现（private+shared, bitset, requeue） |

#### 2.1.4 并发热点分析

**全局共享futex表**（kernel/src/task/futex.rs:269）：
```rust
static SHARED_FUTEX_TABLES: Mutex<FutexTables> = ...;
```
所有共享futex操作序列化到单个全局Mutex，是潜在瓶颈。

**地址空间锁**（kernel/src/task/mod.rs:197）：
```rust
pub aspace: Arc<Mutex<AddrSpace>>,
```
每次页表操作（缺页处理、mmap、munmap）都需要获取此Mutex，同一进程的多线程可能争用。

**IPC全局状态**：消息队列、共享内存使用全局 BTreeMap + Mutex。

### 2.2 多核优化方案

#### 方案1：调度器增强（优先级：高）

**现状**：仅Round-Robin，spawn 时 round-robin 分配核，运行后无动态均衡。

**优化**：
1. 实现 CFS (Completely Fair Scheduler) 或至少 O(1) 调度器
2. 添加周期性负载均衡：每 tick 检查核间负载差异，迁移任务
3. 实现工作窃取：空闲核从繁忙核的运行队列偷取任务
4. 支持 SCHED_FIFO 和 SCHED_OTHER 策略

**实施路径**：在 ax-task 层面修改调度器，或在 StarryOS 层做 wrapper。

#### 方案2：细粒度锁（优先级：高）

**现状**：地址空间使用单个 Mutex。

**优化**：
1. 地址空间拆分为区域级别的锁（per-VMA lock），类似 Linux 6.4+ 的 VMA lock
2. 共享futex表改用分段哈希（sharded HashMap），减少全局争用
3. FD表从 RwLock<FlattenObjects> 改为 per-slot atomic 或更细粒度的并发数据结构

#### 方案3：TLB管理优化（优先级：中）

**现状**：无显式的跨核TLB一致性管理。

**优化**：
1. 实现 lazy TLB flush：context switch 时延迟 TLB 刷新
2. 添加 targeted TLB shootdown：仅通知运行受影响地址空间的核
3. 批量 TLB invalidation：合并多个 munmap/mprotect 操作

#### 方案4：RCU（Read-Copy-Update）（优先级：中）

**现状**：热路径（如进程查找）使用锁。

**优化**：
1. 进程表/任务表查找使用 RCU 保护
2. FD表的读路径使用 RCU（读多写少场景）

#### 方案5：NUMA感知（优先级：低）

**现状**：完全无 NUMA 支持。

**优化**：
1. 添加 NUMA topology 检测
2. 内存分配优先本地 NUMA node
3. 任务调度考虑 NUMA 亲和性

---

## 3. 最值得改进的10个点及改进计划

### 改进点 1：多线程 execve 支持（关键缺陷）

**文件**：kernel/src/syscall/task/execve.rs:50-54

**现状**：多线程进程调用 execve 直接返回 EWOULDBLOCK（AxError::WouldBlock）。Linux 规范要求 execve 应终止同进程的所有其他线程。

**影响**：任何在多线程存活时调用 execve 的程序会失败（如 fork+exec 来自多线程进程中的非主线程、posix_spawn 实现等）。

**改进计划**：
1. 在 execve 中遍历同进程所有线程，发送 SIGKILL
2. 等待所有线程退出
3. 然后替换进程映像

### 改进点 2：Job Control（作业控制）未实现

**文件**：kernel/src/task/signal.rs:29-35（SIGSTOP/SIGCONT 处理），kernel/src/syscall/task/job.rs:41（总 TODO）

**现状**：SIGSTOP 的处理是**错误的** — 当前实现调用 `do_exit(1, true)` 直接**终止进程**而非暂停（signal.rs:31）。SIGCONT 是空操作（signal.rs:34）。终端的前台/后台进程组切换不完整。

**影响**：Ctrl+Z 会**杀死**进程而非挂起，bg/fg 命令无法工作，shell 管道中的进程组管理全部失效。

**改进计划**：
1. 实现 SIGSTOP 导致进程状态转为 STOPPED（需新增任务状态）
2. 实现 SIGCONT 恢复 STOPPED 进程
3. 在 wait4 中正确处理 WUNTRACED/WCONTINUED（报告停止/继续状态）
4. 注：tcsetpgrp/tcgetpgrp 已通过 TIOCSPGRP/TIOCGPGRP ioctl 实现（tty/mod.rs），前台进程组管理基础已有

### 改进点 3：用户/权限系统全为 Stub

**文件**：kernel/src/syscall/sys.rs，kernel/src/syscall/task/ctl.rs

**现状**：所有 UID/GID 操作返回固定值(0)，相当于所有进程都以 root 运行。capget 返回全部 capability 为 MAX（不检查实际权限），capset 仅验证 header 版本后忽略数据。

**影响**：无法正确运行需要权限切换的程序（如 su, sudo, passwd, dropbear SSH）。

**改进计划**：
1. 在 ProcessData 中添加 uid/gid/euid/egid 字段
2. 实现基本的权限检查（文件访问、信号发送）
3. 可选：实现 Linux capabilities 子集

### 改进点 4：POSIX Timer 全为空操作

**文件**：kernel/src/syscall/mod.rs:630

**现状**：timer_create/timer_gettime/timer_settime 直接返回0，不做任何事。timerfd_create 返回 dummy fd。

**影响**：依赖 POSIX 定时器的应用（如高精度定时、超时管理）行为异常。

**改进计划**：
1. 实现 timer_create，维护每进程的定时器列表
2. 实现 timer_settime/timer_gettime
3. 定时到期时发送对应信号（SIGALRM 等）
4. 实现 timerfd_create/timerfd_settime（事件驱动定时器）

### 改进点 5：mmap 缺少非法标志检查和完整的 mprotect

**文件**：kernel/src/syscall/mm/mmap.rs:105, 263

**现状**：mmap 不检查非法标志组合（如同时 MAP_SHARED|MAP_PRIVATE）。mprotect 不支持 PROT_GROWSUP/GROWSDOWN。mremap 实现不完整（mm/mmap.rs:288）。

**改进计划**：
1. 添加 mmap 标志合法性检查
2. 实现 PROT_GROWSUP/GROWSDOWN（栈增长保护）
3. 完善 mremap 支持 MREMAP_MAYMOVE 等

### 改进点 6：IPC 消息队列/共享内存不完整

**文件**：kernel/src/syscall/ipc/msg.rs（5处TODO），kernel/src/syscall/ipc/shm.rs:433

**现状**：
- 消息队列：缺少多种 msgctl 命令的完整实现
- 共享内存：SHM_RND 和 SHM_REMAP 标志未处理
- 无信号量（semaphore）实现

**改进计划**：
1. 补全 msgctl 的 IPC_INFO, MSG_STAT_ANY 等命令
2. 实现 SHM_RND 对齐和 SHM_REMAP
3. 实现 System V 信号量（semget/semop/semctl）

### 改进点 7：Core Dump 和 Stop/Continue 信号处理

**文件**：kernel/src/task/signal.rs:26-34

**现状**：SIGSEGV 等应产生 core dump 的信号仅终止进程（exit code 为 128+signo）。SIGSTOP 错误地终止进程（应暂停），SIGCONT 为空操作。

**改进计划**：
1. 实现基本的 core dump 生成（ELF core format）
2. 完善 stop/continue 状态机（参见改进点2）

### 改进点 8：文件锁（flock/fcntl locking）未实现

**文件**：kernel/src/syscall/fs/fd_ops.rs:310

**现状**：flock() 和 fcntl() 的 F_SETLK/F_GETLK 是空操作。

**影响**：依赖文件锁的程序（数据库、包管理器）存在竞态条件。

**改进计划**：
1. 实现进程级别的文件锁表
2. 支持 LOCK_SH/LOCK_EX/LOCK_UN
3. 实现 fcntl 的字节范围锁

### 改进点 9：/proc 文件系统不完整

**文件**：kernel/src/pseudofs/proc.rs

**现状**：/proc/meminfo 使用硬编码的 dummy 数据。/proc/[pid]/exe 已实现为 symlink（proc.rs 返回 exe_path），loader.rs:282 的 FIXME 注释可能已过时。但缺少 /proc/[pid]/maps, /proc/[pid]/status 等关键接口。

**影响**：许多用户态工具依赖 /proc 获取运行时信息。

**改进计划**：
1. 实现动态的 /proc/meminfo（从实际内存分配器获取数据）
2. 实现 /proc/[pid]/maps（从 AddrSpace 导出 VMA 列表）
3. 实现 /proc/[pid]/status（进程状态、内存使用等）

### 改进点 10：Dummy FD 系统调用应逐步实现

**文件**：kernel/src/syscall/mod.rs:617-628

**现状**：11个系统调用返回 dummy fd（一个无功能的文件描述符），仅为让 QEMU 等软件不崩溃。

**优先实现**：
1. inotify_init1 + inotify_add_watch + inotify_rm_watch（文件监控，影响 busybox 等）
2. timerfd_create + timerfd_settime（事件驱动定时器）
3. io_uring_setup（高性能异步IO，可大幅提升性能）

---

## 4. Linux Syscall支持能力与缺陷分析

### 4.1 总览

StarryOS 在 kernel/src/syscall/mod.rs 的中央分发表中引用了 **210个** 不同的 Sysno 变体（196个 match arm + 1个 catch-all；部分共享处理函数如 fchmodat|fchmodat2、11个 dummy fd 共享一个 handler）。其中 newfstatat/fstatat 是同一 syscall 在不同架构上的名称，因此独立 syscall 为 **209个**。22个仅 x86_64 可用（legacy 接口如 open, stat, mkdir, fork, pipe, select 等），1个仅 riscv64（flush_icache），1个不在 riscv64 上（renameat）。按类别分析如下：

### 4.2 各类别详细分析

#### 4.2.1 文件系统（83个 Sysno 变体，完成度: 95%）

注：包含文件路径操作、FD操作、IO操作、挂载、管道、eventfd、pidfd、memfd、stat、signalfd 相关系统调用，不含 IO 多路复用。

**完整实现**：
- 基本操作：open/openat, close, read/write, lseek, dup/dup2/dup3
- 目录操作：mkdir/mkdirat, rmdir, getdents64, chdir/fchdir, getcwd
- 链接操作：link/linkat, unlink/unlinkat, symlink/symlinkat, readlink/readlinkat
- 重命名：rename/renameat/renameat2
- 文件属性：stat/fstat/lstat/fstatat/statx, chmod/fchmod, chown/fchown
- 高级IO：readv/writev, pread64/pwrite64, preadv/preadv2/pwritev/pwritev2
- 文件传输：sendfile, copy_file_range, splice
- 挂载：mount, umount2

**部分实现/缺陷**：
- **pwritev/pwritev2：存在严重bug** — `sys_pwritev2`（kernel/src/syscall/fs/io.rs:220）调用了 `read_at` 而非 `write_at`，导致写操作实际执行的是读操作。`sys_pwritev` 委托给 `sys_pwritev2`（io.rs:193），因此同样受影响。✅ 已通过自动化测试确认
- **fcntl F_GETFL/F_SETFL：存在逻辑bug** — F_GETFL 从文件权限位（stat.mode）推导访问模式而非保留 open 时的标志：0644 文件无论以 O_RDONLY 还是 O_WRONLY 打开都报告 O_RDWR。F_SETFL 仅处理 O_NONBLOCK，忽略 O_APPEND 等标志。✅ 已通过自动化测试确认（9项测试：4 pass / 5 fail，详见附录 C.3）
- fcntl：F_SETLK/F_GETLK 文件锁未实际实现
- flock：返回 Ok(0) 但不实际加锁（no-op）
- copy_file_range：缺少同文件重叠检查
- sync/syncfs：仅打印警告，无实际刷盘
- memfd_create：标注为 "TODO: correct memfd implementation"
- ioctl：部分 ioctl 命令可能缺失

**完全缺失**：
- inotify_*（文件监控系列，返回 dummy fd）
- fanotify_*（文件访问通知，返回 dummy fd）
- name_to_handle_at / open_by_handle_at
- io_uring_*（异步IO，返回 dummy fd）

#### 4.2.2 内存管理（10个，完成度: 85%）

**完整实现**：
- mmap, munmap, mprotect, brk, mincore, mlock, mlock2, madvise, msync

**部分实现 / 存在 Bug**：
- **mremap：完全不可用**（通过自动化测试确认）。`sys_mremap` 内部调用 `sys_mmap` 时使用 `MAP_PRIVATE` 但缺少 `MAP_ANONYMOUS`（fd=-1），触发 mmap 的 `(ANONYMOUS != fd<=0)` 检查导致 EINVAL。此外还有 4 个设计缺陷：用户 MREMAP_* 标志被变量覆盖、始终移动映射（即使无 MREMAP_MAYMOVE）、缩小映射不保持原地址、忽略原映射类型（shared/file-backed → private anonymous）
- mprotect：缺少 PROT_GROWSUP/GROWSDOWN
- mmap：缺少非法标志检查；`fd <= 0` 判断可能误判 fd=0 (stdin)；huge page 支持可能不完整

**缺失**：
- munlock, munlockall, mlockall
- process_vm_readv, process_vm_writev（跨进程内存访问）
- userfaultfd（用户态缺页处理，返回 dummy fd）

#### 4.2.3 进程/线程管理（完成度: 80%）

注：此处按功能归类，包含附录中 "进程/线程"、"任务管理"、"等待/会话" 等多个小类。

**完整实现**：
- clone/clone3, fork (x86_64), execve, exit, exit_group
- getpid, getppid, gettid
- wait4 (支持 WNOHANG, WUNTRACED, WEXITED 等选项)
- set_tid_address, set_robust_list, get_robust_list
- setsid, getsid, setpgid, getpgid

**部分实现**：
- clone/clone3：命名空间标志仅为 stub（CLONE_NEWPID 等不生效）
- execve：多线程进程 exec 返回 EWOULDBLOCK（关键缺陷）
- sched_setaffinity/getaffinity：仅支持当前线程

**缺失**：
- waitid（更灵活的等待接口）
- execveat（fd-relative exec）

#### 4.2.4 信号处理（12个 + signalfd4，完成度: 90%）

**完整实现**：
- rt_sigaction, rt_sigprocmask, rt_sigpending
- rt_sigreturn, rt_sigsuspend, rt_sigtimedwait
- kill, tkill, tgkill, rt_sigqueueinfo, rt_tgsigqueueinfo
- sigaltstack

**基本实现**：
- signalfd4：基本功能完成，边界情况可能有问题

**部分实现/错误行为**：
- 信号默认动作中 CoreDump 未实现（仅终止进程，exit code 为 128+signo）
- SIGSTOP **行为错误**：当前实现调用 do_exit() 终止进程，而非暂停（kernel/src/task/signal.rs:31）
- SIGCONT 为空操作（什么都不做）

#### 4.2.5 网络（16个，完成度: 85%）

**完整实现**：
- socket, bind, connect, listen, accept/accept4, shutdown
- sendto, sendmsg, recvfrom, recvmsg（注：无独立的 send/recv Sysno，libc 通过 sendto/recvfrom 实现）
- getsockname, getpeername, getsockopt, setsockopt
- socketpair (AF_UNIX)
- 支持协议族：AF_INET (TCP/UDP), AF_UNIX (stream/dgram), AF_VSOCK

**缺失**：
- recvmmsg, sendmmsg（批量消息收发）
- AF_INET6 (IPv6)
- AF_NETLINK（内核-用户空间通信）
- Raw sockets (SOCK_RAW)

#### 4.2.6 时间（6个实现 + 2个睡眠在task_sched + 3个Ok(0)桩，完成度: 70%）

**完整实现**：
- clock_gettime, clock_getres, gettimeofday, times
- nanosleep, clock_nanosleep
- getitimer, setitimer

**空操作/Stub**：
- timer_create, timer_gettime, timer_settime 直接返回0
- timerfd_create 返回 dummy fd
- 缺少 clock_settime, timer_delete, timer_getoverrun

#### 4.2.7 IPC（8个 System V IPC + 管道在文件系统类，完成度: 70%）

**已实现**：
- 消息队列：msgget, msgsnd, msgrcv, msgctl（有5处TODO）
- 共享内存：shmget, shmat, shmdt, shmctl（SHM_RND/SHM_REMAP 未处理）
- 管道（归类在文件系统）：pipe, pipe2

**缺失**：
- System V 信号量：semget, semop, semctl, semtimedop
- POSIX 命名信号量（通过 /dev/shm 文件）

#### 4.2.8 IO多路复用（完成度: 95%）

**完整实现**：
- epoll_create1, epoll_ctl, epoll_pwait, epoll_pwait2
- ppoll, pselect6
- select, poll (仅 x86_64 legacy)

#### 4.2.9 同步（完成度: 85%）

**完整实现**：
- futex (WAIT/WAKE/REQUEUE/BITSET变体)
- membarrier (CMD_GLOBAL, CMD_GLOBAL_EXPEDITED, CMD_PRIVATE_EXPEDITED)

**局限**：
- membarrier 使用 compiler_fence 而非真正的跨核 IPI

#### 4.2.10 系统信息/其他

| 系统调用 | 状态 | 说明 |
|---------|------|------|
| uname | 完整 | 返回系统信息 |
| sysinfo | 部分 | 仅填充 procs 和 mem_unit，其余字段为0 |
| getrandom | 完整 | 随机数 |
| syslog | Stub | 直接返回0，不做任何事 |
| seccomp | Stub | 返回0 |
| prctl | 部分 | 部分子命令 |
| prlimit64 | 部分 | getrlimit/setrlimit 未实现（不在分发表中），仅 prlimit64 可用 |

### 4.3 缺陷汇总

**严重缺陷（影响核心功能）**：
1. **pwritev/pwritev2 实现 bug**：调用 read_at 而非 write_at（io.rs:220），写操作变成读操作。pwritev 委托给 pwritev2，同样受影响。✅ 已通过自动化测试确认
2. **mremap 完全不可用**：内部 sys_mmap 调用缺少 MAP_ANONYMOUS 标志，导致所有 mremap 调用返回 EINVAL。影响所有使用 mremap 的动态内存分配器（如 glibc malloc 的堆段扩展）。✅ 已通过自动化测试确认（9项测试：2 pass / 7 fail，详见附录 C.2）
3. **fcntl F_GETFL 返回错误的访问模式**：从文件权限位推导而非保留 open 标志。0644 文件无论用 O_RDONLY 还是 O_WRONLY 打开，F_GETFL 都报告 O_RDWR。影响所有通过 fcntl 检查 fd 访问模式的��序。✅ 已通过自动化测试确认（9项测试：4 pass / 5 fail，详见附录 C.3）
4. 多线程 execve 不工作
5. Job control (SIGSTOP 错误终止进程 / SIGCONT 空操作)
6. 权限系统全为 stub
7. POSIX timer 全为空操作

**中等缺陷（影响兼容性）**：
8. **F_SETFL 仅处理 O_NONBLOCK**：O_APPEND 等标志被 F_SETFL 忽略，导致运行时无法切换追加模式。✅ 已通过自动化测试确认
9. 文件锁未实现
10. IPC 信号量缺失
11. /proc 使用 dummy 数据
12. IPv6 不支持
13. namespace 仅为 stub

**中等缺陷（影响兼容性）** (续)：
14. **copy_file_range 同文件重叠导致数据损坏**：4096 字节缓冲区逐块复制在前向重叠时覆写源数据。同时 flags 参数被忽略、无常规文件类型检查。✅ 已通过自动化测试确认（7项测试：3 pass / 4 fail，详见附录 C.4）

**轻微缺陷（影响边界情况）**：
15. mmap 标志检查缺失；MAP_ANONYMOUS+fd>0 被错误拒绝。✅ mmap 边界测试确认（8项：7 pass / 1 fail，附录 C.5）
16. scheduler affinity 仅支持当前线程
17. 共享内存 SHM_RND/SHM_REMAP 缺失

---

## 5. Syscall优先实现排序

按 **"影响面 x 实现难度的倒数"** 排序，越靠前表示优先级越高。

### Tier 1：最高优先级（对运行真实应用至关重要）

| 排序 | Syscall | 理由 |
|------|---------|------|
| 1 | **修复 execve 多线程** | 几乎所有非trivial程序都可能是多线程的，这是致命缺陷 |
| 2 | **SIGSTOP/SIGCONT** | Shell 交互核心：Ctrl+Z/fg/bg，测试框架依赖 |
| 3 | **flock / fcntl locking** | 包管理器、数据库、编辑器必需 |
| 4 | **waitid** | 许多 libc 内部使用，比 wait4 更灵活 |
| 5 | **timer_create 系列** | glibc 的 sleep 可能内部使用 POSIX timer |

### Tier 2：高优先级（提升兼容性和应用覆盖）

| 排序 | Syscall | 理由 |
|------|---------|------|
| 6 | **semget/semop/semctl** | 数据库（PostgreSQL, MySQL）和许多服务端程序必需 |
| 7 | **inotify_init1/add_watch/rm_watch** | 文件监控（编辑器热重载、构建系统 watch 模式） |
| 8 | **timerfd_create/settime/gettime** | 事件驱动架构（nginx, redis事件循环） |
| 9 | **实际 uid/gid 实现** | dropbear SSH, su/sudo, 多用户场景 |
| 10 | **execveat** | 现代 glibc fexecve 内部使用 |

### Tier 3：中优先级（性能和现代特性）

| 排序 | Syscall | 理由 |
|------|---------|------|
| 11 | **mremap (完整)** | 动态数组扩容（std::Vec 可能使用）、JIT 编译器 |
| 12 | **recvmmsg/sendmmsg** | 网络高性能场景（DNS服务器、游戏服务器） |
| 13 | **clock_settime** | NTP 时间同步 |
| 14 | **process_vm_readv/writev** | 调试器（GDB/strace）、进程间高效数据传输 |
| 15 | **/proc 完善** | 系统管理工具(top, ps, free)、调试 |

### Tier 4：低优先级（高级特性）

| 排序 | Syscall | 理由 |
|------|---------|------|
| 16 | **io_uring** | 最高性能异步IO，但实现复杂度极高 |
| 17 | **AF_INET6 (IPv6)** | 未来趋势，但目前多数测试用 IPv4 |
| 18 | **seccomp** | 沙箱安全，Docker/Chromium 需要 |
| 19 | **AF_NETLINK** | 网络配置工具(ip, ifconfig)需要 |
| 20 | **namespace 系列** | 容器化支持，实现非常复杂 |

### 排序理由

该排序基于以下原则：
1. **先修后建**：先修复已有功能的严重bug（execve多线程），再添加新功能
2. **覆盖面优先**：优先实现被最多应用依赖的 syscall
3. **libc依赖**：glibc/musl 内部可能隐式依赖的 syscall（timer, waitid）优先
4. **测试基础设施**：能让测试框架正确运行的功能（job control）优先
5. **性价比**：实现简单但影响大的优先（flock 比 io_uring 简单得多）

---

## 6. AI自动迭代编程方法设计

### 6.1 整体架构

```
+-----------------------------------------------------------+
|                   AI 编程迭代循环                           |
|                                                           |
|  +----------+    +----------+    +----------+             |
|  | 1. 分析  | -> | 2. 生成  | -> | 3. 构建  |             |
|  | 目标     |    | 测例     |    | & 测试   |             |
|  +----------+    +----------+    +----------+             |
|       ^                               |                   |
|       |                               v                   |
|  +----------+    +----------+    +----------+             |
|  | 6. 更新  | <- | 5. 验证  | <- | 4. 修复  |             |
|  | 知识库   |    | & 回归   |    | 内核代码 |             |
|  +----------+    +----------+    +----------+             |
+-----------------------------------------------------------+
```

### 6.2 详细方法

#### Phase 1: 目标分析（AI 自动选择下一个目标）

```python
# 伪代码：AI 选择下一个要测试/改进的 syscall
def select_next_target(knowledge_base):
    # 1. 读取当前 syscall 覆盖率
    coverage = parse_syscall_coverage("kernel/src/syscall/mod.rs")

    # 2. 按优先级排序未充分测试的 syscall
    untested = [s for s in coverage if s.test_count < THRESHOLD]
    untested.sort(key=lambda s: s.priority_score, reverse=True)

    # 3. 检查已知的 TODO/FIXME
    todos = grep_todos("kernel/src/")

    # 4. 选择目标
    return untested[0] if untested else todos[0]
```

#### Phase 2: 测例生成（AI 编写用户态测试程序）

AI 根据 Linux man page 和 syscall 语义生成 C 测试程序。测例应包含：

```c
// 示例：自动生成的 mmap 测试
// test_mmap_flags.c - 测试 mmap 标志组合
#include <stdio.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>

#define TEST(name, expr, expected) do { \
    errno = 0; \
    long result = (long)(expr); \
    int err = errno; \
    if ((expected >= 0 && result >= 0) || \
        (expected < 0 && err == -(expected))) { \
        printf("PASS: %s\n", name); \
    } else { \
        printf("FAIL: %s (got %ld, errno=%d=%s, expected %d)\n", \
               name, result, err, strerror(err), expected); \
    } \
} while(0)

int main() {
    // 正常情况
    TEST("anon_private",
         mmap(NULL, 4096, PROT_READ|PROT_WRITE,
              MAP_PRIVATE|MAP_ANONYMOUS, -1, 0),
         0);

    // 错误情况: 同时设 MAP_SHARED 和 MAP_PRIVATE 应返回 EINVAL
    TEST("shared_and_private",
         mmap(NULL, 4096, PROT_READ,
              MAP_SHARED|MAP_PRIVATE|MAP_ANONYMOUS, -1, 0),
         -EINVAL);

    // 边界情况: size=0
    TEST("zero_size",
         mmap(NULL, 0, PROT_READ, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0),
         -EINVAL);

    return 0;
}
```

#### Phase 3: 自动构建与测试

```bash
# auto_test.sh - 自动构建和运行测试的脚本框架

# 1. 交叉编译测试程序
riscv64-linux-gnu-gcc -static -o test_program test_mmap_flags.c

# 2. 将测试程序放入 rootfs 镜像
# (mount + copy + umount rootfs image)

# 3. 修改 init 脚本运行测试
# 设置 init.sh 执行测试程序并输出结果

# 4. 构建并运行 StarryOS
# make ARCH=riscv64 run 2>&1 | tee test_output.log

# 5. 解析输出
# grep -E "^(PASS|FAIL):" test_output.log > results.txt
```

#### Phase 4: AI 分析失败原因并修复内核

```
AI 收到失败测例的输出后：
1. 定位失败的 syscall
2. 读取对应的内核源码
3. 对比 Linux man page 的规范行为
4. 生成修复补丁
5. 重新运行测试验证
```

#### Phase 5: 回归测试

确保修复不破坏已有功能，运行所有已通过的测试用例。

#### Phase 6: 更新知识库

```json
{
    "syscall": "mmap",
    "tested_flags": ["MAP_PRIVATE", "MAP_SHARED", "MAP_ANONYMOUS", "MAP_FIXED"],
    "known_bugs": [
        {
            "description": "MAP_SHARED|MAP_PRIVATE not returning EINVAL",
            "status": "fixed",
            "commit": "abc1234"
        }
    ],
    "test_files": ["test_mmap_flags.c", "test_mmap_file.c"],
    "coverage_score": 0.85
}
```

### 6.3 具体实施方案（用 Claude Code 实现）

**步骤1**: AI 读取 syscall 分发表
- 解析 kernel/src/syscall/mod.rs 获取所有已实现 syscall
- 解析 TODO/FIXME 获取已知问题

**步骤2**: 生成测试矩阵
对每个 syscall 生成：
- 正常路径测试（happy path）
- 错误路径测试（invalid args -> 预期 errno）
- 边界情况测试（空指针、零长度、最大值等）
- 并发测试（多线程同时调用）

**步骤3**: 交叉编译 + 注入 rootfs
- 使用 riscv64-linux-gnu-gcc -static 编译
- 使用 losetup + mount 修改 disk image
- 或者使用 QEMU user-mode 直接运行（如果可用）

**步骤4**: 运行 + 采集结果
- make ARCH=riscv64 run
- 解析串口输出中的 PASS/FAIL 标记
- 收集 kernel panic / warning 日志

**步骤5**: AI 分析 + 修复
- 对每个 FAIL，AI 读取对应源码
- 参考 Linux kernel 源码或 man page
- 生成修复补丁并验证

**步骤6**: 迭代
- 回到步骤2，针对修复后的新行为生成更多测试
- 逐步提高覆盖率

### 6.4 关键设计决策

**为什么选择 C 测试而非 Rust 测试？**
- C 测试可以直接使用 libc syscall wrapper，更接近真实应用场景
- 静态链接后二进制小，容易放入 rootfs
- 与 Linux 测试套件（如 LTP）格式兼容

**为什么需要知识库？**
- 避免 AI 重复测试已知通过/已知失败的情况
- 跟踪修复历史，防止回归
- 可以作为项目文档的一部分

**自动化程度考量**：
- 全自动：测例生成 -> 编译 -> 运行 -> 结果采集
- 半自动：Bug分析和修复建议由 AI 生成，人工review后合入
- 这种混合模式兼顾了效率和安全性

---

## 7. 面试准备：核心问题与回答要点

### 7.1 架构设计

**Q: StarryOS 与 ArceOS 的关系是什么？为什么选择在 ArceOS 之上构建？**

A: ArceOS 是一个模块化的 library OS 框架，提供了 HAL（硬件抽象层）、调度器、内存管理、文件系统和网络栈等基础组件。StarryOS 将 ArceOS 作为"基座"，在其上构建宏内核功能层（进程管理、信号系统、Linux syscall 兼容层、伪文件系统等）。

这种设计的优势：
1. **复用成熟组件**：不需要从头实现驱动、调度器等底层功能
2. **模块化**：可以通过 feature flag 裁剪不需要的功能
3. **跨架构移植性**：ArceOS 的 HAL 层已经处理了多架构差异
4. **渐进开发**：从 unikernel 渐进到宏内核，可以逐步添加功能

劣势：
1. 受限于 ArceOS 的抽象，某些优化（如调度器定制）需要修改上游
2. 两层代码的边界有时模糊，调试链条较长

**Q: 用户态和内核态如何切换？各架构有何不同？**

A:
- **RISC-V**: 通过 ecall 触发 Environment Call 异常进入 S-mode，sret 返回 U-mode。仅有单个 SATP 寄存器，内核需映射在用户页表中（通过 `copy_from_kernel` 复制内核页表条目）。
- **AArch64**: 通过 svc 指令进入 EL1，eret 返回 EL0。使用 TTBR0_EL1 (用户)和 TTBR1_EL1 (内核)两张独立页表，无需复制。
- **LoongArch64**: 类似 AArch64 使用分离页表（PGDL/PGDH）。
- **x86_64**: 通过 syscall 指令进入 Ring 0，sysret 返回 Ring 3。内核页表需映射到用户页表的高半部分。

关键代码：kernel/src/config/ 定义各架构的地址空间布局，kernel/src/mm/loader.rs 处理架构差异。

### 7.2 组件关联

**Q: 进程创建 (fork/clone) 的完整流程涉及哪些组件？**

A: 调用链：
```
sys_clone() [kernel/src/syscall/task/clone.rs]
  -> 解析 clone_flags
  -> 复制/共享地址空间 (CLONE_VM 决定)
      -> AddrSpace::try_clone() [kernel/src/mm/aspace/mod.rs:348]
      -> CoW backend 标记页面 [backend/cow.rs]
  -> 复制/共享文件描述符表 (CLONE_FILES)
      -> FD_TABLE scope clone [kernel/src/file/]
  -> 复制/共享信号处理器 (CLONE_SIGHAND)
      -> SignalActions clone [starry-signal crate]
  -> 创建新 ProcessData 和 Thread
      -> ProcessData::new() [kernel/src/task/mod.rs]
  -> 创建 Process 节点
      -> Process::new() [starry-process crate]
  -> 创建新 UserContext (子进程返回值=0)
      -> new_user_task() [kernel/src/task/user.rs]
  -> spawn_task() -> 提交到 ax-task 调度器
```

涉及组件：syscall层 -> 内存管理(CoW) -> 文件系统(FD) -> 信号系统 -> 进程管理 -> 调度器

**Q: 缺页处理 (page fault) 的流程？**

```
用户态访问未映射地址
  -> CPU 产生 Page Fault 异常
  -> ax-hal trap handler
  -> kernel/src/task/user.rs (line 32-41)
  -> 获取 ProcessData.aspace Mutex (line 33)
  -> AddrSpace::handle_page_fault(addr, flags)
  -> 根据 backend 类型分发（populate 方法）：
      -> Backend::Linear: 线性偏移映射（静态物理到虚拟）
      -> Backend::Cow: 分配新页并复制（写时复制触发），更新映射为可写
      -> Backend::File: 从文件读取数据到新分配的页
      -> Backend::Shared: 映射共享物理页
  -> 返回用户态
```

### 7.3 核心技术

**Q: Futex 的实现原理？为什么需要区分 private 和 shared？**

A: Futex (Fast Userspace Mutex) 的核心思想是：无竞争时在用户态通过原子操作完成，有竞争时才进入内核等待。

实现（kernel/src/task/futex.rs）：
- FutexTable: 哈希表，key 是用户态地址或 SharedPages 偏移
- WaitQueue: 每个地址对应一个等待队列（SpinNoIrq<VecDeque<(Waker, u32)>>，u32 为 bitset 掩码）
- FUTEX_WAIT: 检查用户态值是否匹配 -> 匹配则加入等待队列 -> sleep
- FUTEX_WAKE: 从等待队列唤醒 N 个线程

Private vs Shared 区分：
- **Private futex**: 仅同一进程的线程可见，用虚拟地址做 key。简单高效。
- **Shared futex**: 跨进程（通过 mmap MAP_SHARED），需要用物理页偏移做 key，确保不同进程的不同虚拟地址映射到同一个等待队列。

全局共享futex表的设计（static SHARED_FUTEX_TABLES）是当前的性能瓶颈之一。

**Q: 信号(Signal)投递机制？如何处理 handler 的用户态执行？**

A:
1. **发送**：send_signal_to_thread() 将信号加入目标线程的 pending 集合，调用 task.interrupt() 打断可能的 sleep
2. **投递时机**：从内核态返回用户态之前检查 pending signals
3. **Handler 执行**：
   - 在用户栈上构造 SignalFrame（保存当前寄存器上下文）
   - 将 PC 指向用户注册的 handler 函数
   - 将 return address 指向 **signal trampoline**（映射在固定地址 0x6000_1000）
   - trampoline 代码执行 rt_sigreturn 系统调用
4. **恢复**：sys_rt_sigreturn() 从栈上的 SignalFrame 恢复原始上下文

这个设计精巧之处在于：handler 在用户态执行（安全），但通过 trampoline 机制自动回到内核恢复原始执行流。

**Q: 并发和中断对 OS 设计的影响？**

A: StarryOS 面临的并发挑战：

1. **中断 vs 任务**：使用 SpinNoIrq（关中断自旋锁）保护可能在中断上下文中访问的数据（如 futex wait queue），防止死锁。
2. **多核 vs 共享数据**：
   - 地址空间：Mutex<AddrSpace>，同进程多线程争用
   - FD表：RwLock 允许并发读（open/read/write 不互斥）
   - 进程表：全局 TASK_TABLE 需要原子操作或锁
3. **信号 vs 执行流**：信号可能打断任何系统调用，需要 EINTR 处理和 SA_RESTART 语义
4. **外设中断**：通过 register_irq_waker() 将中断转化为 waker，与 async/poll 模型结合

设计原则：
- 关中断时间最短化（SpinNoIrq 仅保护短临界区）
- 尽量使用无锁原子操作（exit flag, heap top 等用 Atomic）
- 长操作使用可阻塞的 Mutex（地址空间操作）
- 中断与任务解耦（waker 模型）

### 7.4 知识点关联

**Q: StarryOS 的 Copy-on-Write 如何与 fork + exec 配合？**

A: 这是经典的 UNIX 优化：
1. fork() 时不复制父进程的物理页，而是将所有页标记为只读，使用 CoW backend
2. 任一进程写入时触发 Page Fault -> CoW handler 分配新页并复制 -> 恢复写权限
3. exec() 时调用 `uspace.clear()` 清空所有映射（CoW 页面的物理页引用计数减少），然后重新加载新 ELF
4. 优化效果：fork+exec 组合几乎不产生物理页复制

在 StarryOS 中：
- kernel/src/syscall/task/clone.rs:205：非 CLONE_VM 时通过 `aspace.try_clone()` 创建 CoW 副本
- kernel/src/mm/aspace/backend/cow.rs：CoW 后端实现
- kernel/src/syscall/task/execve.rs + kernel/src/mm/loader.rs:195：exec 时清空现有地址空间（`uspace.clear()`）后重新加载 ELF

**Q: 各模块之间的依赖关系？**

```
调度器(ax-task)
  ^ 依赖
进程管理(starry-process) <-> 信号系统(starry-signal)
  ^ 依赖                       ^ 依赖
  虚拟内存(starry-vm, ax-mm)   信号投递需要检查掩码
  ^ 依赖
  文件系统(ax-fs) <-> 网络(axnet)
  ^ 依赖
  设备驱动(ax-driver) -> HAL(ax-hal) -> 硬件
```

关键循环依赖（通过间接引用解决）：
- 信号投递需要任务管理（找到目标线程）
- 任务退出需要信号系统（发送 SIGCHLD）
- mmap 需要文件系统（文件映射）
- 文件系统需要内存管理（缓冲区分配）

StarryOS 通过 Arc/Weak 引用和 trait objects 打破循环。

### 7.5 深入子系统

**Q: ELF 加载器如何支持动态链接？**

A: kernel/src/mm/loader.rs 实现了两阶段加载：
1. 解析应用 ELF 的 PT_INTERP 段，提取动态链接器路径（如 `/lib/ld-linux-riscv64-lp64d.so.1`）
2. 将应用映射到 USER_SPACE_BASE，将动态链接器映射到 USER_INTERP_BASE
3. 设置辅助向量（auxv）：AT_PHDR, AT_ENTRY, AT_BASE, AT_PAGESZ 等
4. 入口点设为动态链接器的入口，由 ld.so 完成符号解析后跳转到应用

额外特性：
- 32 条目的 LRU 缓存（uluru::LRUCache）避免重复解析 ELF headers
- 支持 `#!` 脚本解释器：如果 ELF 解析失败且文件以 `#!` 开头，递归加载解释器
- `.sh` 文件自动重定向到 `/bin/sh`

**Q: TTY/PTY 子系统的行纪律 (line discipline) 如何工作？**

A: kernel/src/pseudofs/dev/tty/terminal/ldisc.rs 实现了三种处理模式：
- **Manual**: 仅在 read() 调用时处理输入（简单场景）
- **External**: 独立的 `tty-reader` 任务实时处理输入（主模式）
- **None**: PTY master 使用，不做处理直接传递

Canonical 模式（ICANON）下：输入按行缓冲，支持 backspace (VERASE)、行清除 (VKILL)、行结束 (EOL/EOF)
Non-canonical 模式下：立即返回输入，受 VMIN/VTIME 控制

信号处理：检测 VINTR (Ctrl+C → SIGINT)、VQUIT (Ctrl+\ → SIGQUIT)，向前台进程组发送信号

PTY 实现：打开 /dev/ptmx 创建 master/slave 对，通过两个 4096 字节环形缓冲区双向通信。行纪律仅在 slave 端处理，master 端为原始数据通道。

**Q: 外设中断如何影响 OS 的设计？以 TTY 为例。**

A: StarryOS 使用中断-waker 解耦模型：
1. 硬件中断到达 → ax-hal 的中断处理器被调用
2. N_TTY 通过 `register_irq_waker(irq, &waker)` 注册中断唤醒器
3. 中断触发时，waker 唤醒 TTY reader 任务（而非在中断上下文中直接处理数据）
4. TTY reader 任务在普通任务上下文中读取设备数据、执行行纪律处理

好处：
- 中断处理极短（仅唤醒，不做业务逻辑）
- 避免在中断上下文中持锁（防止死锁）
- 与 async/await 模型自然融合（SpinNoIrq 保护共享状态）

### 7.6 已有测试基础设施

StarryOS 有多层测试：
- **CI 流水线**（.github/workflows/test.yml）：4 架构矩阵测试（riscv64, aarch64, loongarch64, x86_64）
- **QEMU 启动测试**（scripts/ci-test.py）：TCP 串口连接 QEMU，验证 BusyBox shell 能正常启动并响应命令
- **代码质量检查**（.github/workflows/check.yml）：cargo fmt + clippy
- **Make 目标**：`make ci-test` 运行自动化 QEMU 测试，`make debug` 启动 GDB 调试

局限：当前测试仅验证"能启动并进入 shell"，缺乏 syscall 级别的正确性测试。这正是第 6 节 AI 自动迭代方法要解决的问题。

### 7.7 内存分配器架构

**Q: StarryOS 的内存分配分几层？各层职责是什么？**

A: 三层架构，由 ArceOS 的 ax-alloc 提供统一入口：

```
ax-alloc (全局分配器入口，通过 global_allocator() 访问)
  |
  +-- Slab 分配器 (ax_slab_allocator v0.4.0，通过 alloc-slab feature 启用)
  |     用途：小对象堆分配，服务 Box::new, Arc::new, Vec::new 等
  |     当 slab 需要补充时，从 buddy 分配器申请新页
  |
  +-- Buddy 页分配器 (buddy_system_allocator v0.12.0，通过 page-alloc-4g feature 启用)
        用途：物理页帧分配，4GB 页池
        用于用户地址空间映射 (alloc_frame → global_allocator().alloc_pages())
```

**用户空间页帧分配**（kernel/src/mm/aspace/backend/mod.rs）：
- `alloc_frame(zeroed, size)` — 分配物理页帧，参数 `zeroed=true` 时清零（防止信息泄漏）
- `dealloc_frame(frame, align)` — 释放物理页帧
- 通过 `UsageKind::VirtMem` 标记分配用途

**各 Backend 的分配策略**：
- **CowBackend**：懒分配 — 页帧在首次写入触发 page fault 时才分配（CoW 语义）
- **SharedBackend**：立即分配 — `SharedPages::new()` 一次性分配所有页帧并清零
- **FileBackend**：按需加载 — 从文件缓存读取数据到新分配的页帧
- **LinearBackend**：不分配 — 静态偏移映射（用于内核线性区域）

**CoW 引用计数**（kernel/src/mm/aspace/backend/cow.rs）：
- 全局 `FRAME_TABLE: SpinNoIrq<FrameTableRefCount>`（内部为 `BTreeMap<PhysAddr, Arc<SpinNoIrq<FrameRefCnt>>>`）跟踪每页引用
- 引用计数为 u8（最大 255 次共享）
- 单引用时直接升级权限（无需复制），多引用时分配新页并复制

**可选内存追踪**（kernel/src/pseudofs/dev/memtrack.rs）：
- 通过 `memtrack` feature 启用，在 /dev/memtrack 写入 "start\n" / "end\n" 控制
- 结合 DWARF 栈回溯，按分配来源分类统计（ELF cache、进程数据、ext4 inode 等）

### 7.8 网络栈架构

**Q: StarryOS 的网络栈如何组织？支持哪些协议和功能？**

A: 网络栈分两层：StarryOS 内核处理 syscall 接口和 Unix 域 socket，底层 TCP/IP 由 axnet（基于 starry-smoltcp v0.12.1-preview.1）实现。

**Socket 类型**（kernel/src/syscall/net/socket.rs）：

| 协议族 | 类型 | 实现 |
|--------|------|------|
| AF_INET | SOCK_STREAM | TcpSocket（axnet，基于 smoltcp TCP） |
| AF_INET | SOCK_DGRAM | UdpSocket（axnet，基于 smoltcp UDP） |
| AF_UNIX | SOCK_STREAM | UnixSocket + StreamTransport（内核内实现） |
| AF_UNIX | SOCK_DGRAM | UnixSocket + DgramTransport（内核内实现） |
| AF_VSOCK | SOCK_STREAM | VsockSocket（可选 vsock feature） |

**关键设计决策**：
- **Unix 域 socket 不经过 smoltcp TCP/IP 栈**，由 axnet 的 UnixSocket + StreamTransport/DgramTransport 实现进程间直接内存传递
- **支持 SCM_RIGHTS**（FD 传递）：通过 cmsg.rs 实现 Unix socket 的辅助数据，可在进程间传递文件描述符
- **socketpair 仅支持 AF_UNIX**

**Socket 选项**（kernel/src/syscall/net/opt.rs）：
- SOL_SOCKET：SO_REUSEADDR, SO_ERROR, SO_SNDBUF, SO_RCVBUF, SO_KEEPALIVE, SO_RCVTIMEO, SO_SNDTIMEO, SO_PASSCRED, SO_PEERCRED
- IPPROTO_TCP：TCP_NODELAY, TCP_MAXSEG, TCP_INFO
- IPPROTO_IP：IP_TTL

**不支持**：
- AF_INET6（IPv6）— 地址解析代码存在但 socket 创建不匹配 AF_INET6
- AF_NETLINK — 网络管理工具（ip, ifconfig）所需
- SOCK_RAW — 原始套接字
- 大部分 socket ioctl — 仅 FIONBIO（设置非阻塞）在 sys_ioctl 中全局处理

---

## 附录 A：210个 Sysno 变体（209个独立 syscall）完整清单

注：因架构条件编译（#[cfg]），单一架构上可用的 syscall 数量少于总数：
- x86_64 独有 **22个** legacy syscall：access, arch_prctl, chmod, chown, dup2, fork, lchown, link, lstat, mkdir, open, pipe, poll, readlink, rename, rmdir, select, stat, symlink, unlink, utime, utimes
- riscv64 独有 **1个**：riscv_flush_icache
- renameat **不在** riscv64 上
- newfstatat (x86_64/riscv64) 和 fstatat (aarch64/loongarch64) 是同一 syscall 的不同名称

### 文件系统及IO (含路径操作/FD操作/IO/管道/eventfd/pidfd/memfd/stat/signalfd)
```
ioctl, chdir, fchdir, chroot, mkdir*, mkdirat, getdents64, link*, linkat,
rmdir*, unlink*, unlinkat, getcwd, symlink*, symlinkat, rename*, renameat**,
renameat2, sync, syncfs, chown*, lchown*, fchown, fchownat, chmod*, fchmod,
fchmodat/fchmodat2, readlink*, readlinkat, utime*, utimes*, utimensat,
open*, openat, close, close_range, dup, dup2*, dup3, fcntl, flock,
read, readv, write, writev, lseek, truncate, ftruncate, fallocate,
fsync, fdatasync, fadvise64, pread64, pwrite64, preadv, pwritev,
preadv2, pwritev2, sendfile, copy_file_range, splice,
stat*, fstat, lstat*, newfstatat/fstatat, statx, access*, faccessat/faccessat2,
statfs, fstatfs, mount, umount2, pipe2, pipe*, eventfd2, pidfd_open,
pidfd_getfd, pidfd_send_signal, memfd_create, signalfd4
```
（带 * 仅 x86_64，带 ** 非 riscv64）

### 内存管理 (10)
```
brk, mmap, munmap, mprotect, mincore, mremap, madvise, msync, mlock, mlock2
```

### 进程/线程 (18)
```
getpid, getppid, gettid, getrusage, sched_yield, nanosleep, clock_nanosleep,
sched_getaffinity, sched_setaffinity, sched_getscheduler, sched_setscheduler,
sched_getparam, getpriority, execve, set_tid_address, arch_prctl*, prctl,
prlimit64
```

### 任务管理 (12)
```
capget, capset, umask, setreuid, setresuid, setresgid, get_mempolicy,
clone, clone3, fork*, exit, exit_group
```

### 等待/会话 (5)
```
wait4, getsid, setsid, getpgid, setpgid
```

### 信号 (12)
```
rt_sigprocmask, rt_sigaction, rt_sigpending, rt_sigreturn, rt_sigtimedwait,
rt_sigsuspend, kill, tkill, tgkill, rt_sigqueueinfo, rt_tgsigqueueinfo,
sigaltstack
```
注：signalfd4 在 "信号文件描述符" 单独分类（1个）

### 同步/Futex (4)
```
futex, get_robust_list, set_robust_list, membarrier
```

### 用户/系统 (14)
```
getuid, geteuid, getgid, getegid, setuid, setgid, getgroups, setgroups,
uname, sysinfo, syslog, getrandom, seccomp, riscv_flush_icache (riscv64 only)
```

### 网络 (16)
```
socket, socketpair, bind, connect, getsockname, getpeername, listen,
accept, accept4, shutdown, sendto, recvfrom, sendmsg, recvmsg,
getsockopt, setsockopt
```

### 时间 (6)
```
gettimeofday, times, clock_gettime, clock_getres, getitimer, setitimer
```

### IPC (8)
```
msgget, msgsnd, msgrcv, msgctl, shmget, shmat, shmctl, shmdt
```

### IO多路复用 (8)
```
poll*, ppoll, select*, pselect6, epoll_create1, epoll_ctl, epoll_pwait, epoll_pwait2
```

### Dummy FD (11)
```
timerfd_create, fanotify_init, inotify_init1, userfaultfd,
perf_event_open, io_uring_setup, bpf, fsopen, fspick, open_tree, memfd_secret
```

### 空操作 (3)
```
timer_create, timer_gettime, timer_settime
```

---

## 附录 B：源码中的 TODO/FIXME 统计

共发现 **61处** TODO/FIXME 标记（TODO注释: 43处，FIXME注释: 17处，todo!()宏: 1处）。按重要性分类：

**关键** (影响正确性)：
- **io.rs:220 - pwritev/pwritev2 实现 bug**：sys_pwritev2 调用 read_at 而非 write_at（与 preadv2 代码完全相同，copy-paste 错误）。sys_pwritev 委托给 sys_pwritev2（io.rs:193），同样受影响。✅ 已通过自动化测试确认
- **mmap.rs:282-318 - mremap 完全不可用（5个独立 bug）**：✅ 已通过自动化测试确认
  1. sys_mmap 调用缺少 MAP_ANONYMOUS → 所有 mremap 返回 EINVAL
  2. 用户 MREMAP_* 标志被第300行变量覆盖（变量名遮蔽）
  3. 始终创建新映射移动数据（即使无 MREMAP_MAYMOVE）
  4. 缩小映射不保持原地址（Linux 要求缩小必须原地）
  5. 新映射始终为 PRIVATE ANONYMOUS（忽略原 shared/file-backed 类型）
- execve.rs:50-54 - 多线程 exec 未处理（返回 EWOULDBLOCK）
- job.rs:41 - 作业控制未实现
- signal.rs:26-34 - core dump / stop / continue 未实现（stop 会错误终止进程）
- mmap.rs:105 - 非法 mmap 标志未检查
- mmap.rs:263 - PROT_GROWSUP/DOWN 未实现
- ldisc.rs:347 - **todo!() 宏**：当 termios VTIME > 0 时会 panic（运行时崩溃）

**重要** (影响兼容性)：
- fd_ops.rs:310 - flock 未实现
- memfd.rs:13 - memfd 实现不正确
- schedule.rs:96,124 - affinity 仅支持当前线程
- shm.rs:433 - SHM_RND/SHM_REMAP 未处理
- wait.rs:81 - WALL/WCLONE 支持缺失

**改善** (性能/质量)：
- timer.rs:133 - 抢占不影响定时器状态
- access.rs:84 - 用户内存访问效率低
- backend/shared.rs:53 - 不支持部分范围 map/unmap
- tmp.rs:428 - 原子性问题
- entry.rs:74 - 未等待所有进程结束
- poll.rs:111 - signal 处理缺失
- dir.rs:90 - 缓存一致性问题
- net.rs:35 - socket 的 stat 未实现
- tty/mod.rs:118,126,171 - 输出排空和 session leader SIGHUP 处理
- tty/terminal/ldisc.rs:347 - 存在 todo!() 宏（运行时 panic）

**代码质量** (AnyBitPattern/Zeroable 相关的 unsafe)：
- 9处 FIXME: AnyBitPattern（fs/ctl.rs x3, task/ctl.rs, schedule.rs, futex.rs, time.rs, resources.rs, loop.rs）
- 3处 FIXME: Zeroable（stat.rs, sys.rs, resources.rs）

---

## 附录 C：自动化 Syscall 测试结果

> 测试框架：自定义 C 测试工具链（`tests/cases/starry_test.h`），通过 riscv64-linux-musl-gcc 交叉编译，注入 rootfs ext4 镜像，QEMU 启动执行。
> 测试日期：2026-04-12

### C.1 pwritev2 测试 (`tests/cases/test_pwritev2.c`)

| 测试 | 结果 | 说明 |
|------|------|------|
| `pwrite_basic` | **PASS** | 基准：pwrite 正常工作 |
| `pwritev_writes_data` | **FAIL** | pwritev 写入后读回数据不匹配（bug: 调用 read_at 而非 write_at） |
| `pwritev2_writes_data` | **FAIL** | 直接 pwritev2 同样失败（同一 bug） |

**根因**：`kernel/src/syscall/fs/io.rs:220`，`sys_pwritev2` 从 `sys_preadv2` 复制而来，忘记将 `read_at` 改为 `write_at`。

### C.2 mremap 测试 (`tests/cases/test_mremap.c`)

**总结：2 PASS / 7 FAIL — mremap 完全不可用**

| 测试 | 结果 | 说明 |
|------|------|------|
| `mremap_not_einval` | **FAIL** | mremap 能否成功执行？不能 — 所有调用返回 EINVAL |
| `basic_grow_data_preserved` | **FAIL** | 扩展映射后数据保持完整性 |
| `grow_new_pages_zeroed` | **FAIL** | 扩展后新页面应为零 |
| `basic_shrink` | **FAIL** | 缩小映射 |
| `shrink_returns_same_addr` | **FAIL** | Linux 要求缩小映射返回相同地址 |
| `no_maymove_must_not_move` | **FAIL** | 无 MREMAP_MAYMOVE 标志不应移动映射 |
| `data_pattern_integrity` | **FAIL** | 逐字节数据完整性验证 |
| `invalid_unaligned_addr` | **PASS** | 非页对齐地址正确返回 EINVAL |
| `invalid_zero_new_size` | **PASS** | new_size=0 正确返回 EINVAL |

**发现的 5 个 Bug**：

#### Bug #1（严重）：缺少 MAP_ANONYMOUS 标志

`kernel/src/syscall/mm/mmap.rs:302-306`：

```rust
let new_addr = sys_mmap(
    addr.as_usize(),
    new_size,
    flags.bits() as _,
    MmapFlags::PRIVATE.bits(),  // ← 缺少 MAP_ANONYMOUS
    -1,                          // ← fd=-1 要求设置 ANONYMOUS
    0,
)?;
```

`sys_mmap` 在第123行检查 `map_flags.contains(ANONYMOUS) != (fd <= 0)`，由于 `MAP_ANONYMOUS` 未设置但 `fd=-1 <= 0`，条件为 `false != true = true`，返回 `EINVAL`。

**修复**：将 `MmapFlags::PRIVATE.bits()` 改为 `(MmapFlags::PRIVATE | MmapFlags::ANONYMOUS).bits()`。

#### Bug #2：MREMAP_* 标志被变量覆盖

`mmap.rs:282,300`：

```rust
pub fn sys_mremap(addr: usize, old_size: usize, new_size: usize, flags: u32) -> AxResult<isize> {
    // ...
    let flags = aspace.find_area(addr).ok_or(AxError::NoMemory)?.flags();
    //  ^^^^^^ 遮蔽了函数参数 flags: u32（MREMAP_MAYMOVE=1, MREMAP_FIXED=2 等）
```

用户传入的 `MREMAP_MAYMOVE`、`MREMAP_FIXED`、`MREMAP_DONTUNMAP` 标志完全丢失。

#### Bug #3：始终移动映射

即使不设置 `MREMAP_MAYMOVE`，实现仍然通过 `mmap + memcpy + munmap` 创建新映射。Linux 行为：无 `MREMAP_MAYMOVE` 时，扩展必须原地进行或返回 `ENOMEM`。

#### Bug #4：缩小映射不保持原地址

Linux 规定缩小映射始终返回相同地址（直接截断尾部页面）。当前实现分配新内存区域再复制，导致地址改变。

#### Bug #5：忽略原映射类型

新映射始终为 `MAP_PRIVATE | MAP_ANONYMOUS`，即使原映射为 `MAP_SHARED` 或文件映射。这意味着：
- 共享映射变为私有（其他进程无法看到修改）
- 文件映射变为匿名（文件数据在新区域丢失）

#### MappingFlags → PROT 转换的意外正确性

值得注意的是，`flags.bits() as _` 作为 `prot` 参数传递给 `sys_mmap` 碰巧是正确的：`MappingFlags::READ/WRITE/EXECUTE` 的位值 (0x1/0x2/0x4) 恰好与 `PROT_READ/PROT_WRITE/PROT_EXEC` 相同。多余的 `MappingFlags::USER` (0x8) 被 `from_bits_truncate` 截断，然后在 `From<MmapProt> for MappingFlags` 转换中自动重新添加。这是巧合而非设计。

### C.3 fcntl F_GETFL/F_SETFL 测试 (`tests/cases/test_fcntl_getfl.c`)

**总结：4 PASS / 5 FAIL — F_GETFL 返回错误的访问模式**

| 测试 | 结果 | 说明 |
|------|------|------|
| `rdonly_on_rw_file` | **FAIL** | O_RDONLY 打开 0644 文件 → F_GETFL 报告 O_RDWR (accmode=2) |
| `wronly_on_rw_file` | **FAIL** | O_WRONLY 打开 0644 文件 → F_GETFL 报告 O_RDWR (accmode=2) |
| `rdwr_on_rw_file` | **PASS** | O_RDWR 打开 0644 文件 → F_GETFL 报告 O_RDWR（巧合正确） |
| `append_flag_preserved` | **FAIL** | O_WRONLY\|O_APPEND\|O_CREAT\|O_TRUNC 打开失败（可能为 FS 层 bug） |
| `setfl_nonblock_roundtrip` | **PASS** | F_SETFL O_NONBLOCK 正确设置和查询 |
| `setfl_append_via_fcntl` | **FAIL** | F_SETFL O_APPEND 无效果 — 数据被覆盖而非追加 |
| `getfl_pipe_read_end` | **PASS** | 管道读端 F_GETFL 正确返回 O_RDONLY |
| `getfl_pipe_write_end` | **PASS** | 管道写端 F_GETFL 正确返回 O_WRONLY |
| `accmode_not_changed_by_setfl` | **FAIL** | O_RDONLY fd 上 F_GETFL 报告 O_RDWR（同 Bug #1） |

**发现的 3 个 Bug**：

#### Bug #1（严重）：F_GETFL 从文件权限推导访问模式

`kernel/src/syscall/fs/fd_ops.rs:256-273`：

```rust
F_GETFL => {
    let f = get_file_like(fd)?;
    let mut ret = 0;
    if f.nonblocking() { ret |= O_NONBLOCK; }
    let perm = NodePermission::from_bits_truncate(f.stat()?.mode as _);
    if perm.contains(NodePermission::OWNER_WRITE) {        // ← 检查文件权限
        if perm.contains(NodePermission::OWNER_READ) {
            ret |= O_RDWR;                                  // ← 而非 open 时的标志
        } else {
            ret |= O_WRONLY;
        }
    }
    Ok(ret as _)
}
```

**问题**：对于 0644 (rw-r--r--) 文件，`OWNER_WRITE` 和 `OWNER_READ` 都为 true，因此始终返回 O_RDWR — 无论文件是以 O_RDONLY 还是 O_WRONLY 打开的。

**修复方向**：File 结构体应保存 open 时的 flags，F_GETFL 直接返回保存的 flags。

**管道测试 PASS 的原因**：管道的 stat.mode 碰巧与访问模式一致 — 读端 mode 无 OWNER_WRITE → 返回 0 (O_RDONLY)，写端 mode 可能设置了 OWNER_WRITE 但无 OWNER_READ → 返回 O_WRONLY。这是巧合而非正确实现。

#### Bug #2：O_APPEND 完全不被追踪

`open()` 传入的 O_APPEND 标志不会被 F_GETFL 返回。测试 `append_flag_preserved` 更进一步发现：`open(path, O_WRONLY|O_CREAT|O_TRUNC|O_APPEND, 0644)` 本身返回失败（fd < 0），暗示 O_APPEND 与 O_TRUNC 的组合在 FS 层可能存在额外 bug。

#### Bug #3：F_SETFL 仅处理 O_NONBLOCK

`kernel/src/syscall/fs/fd_ops.rs:252-254`：

```rust
F_SETFL => {
    get_file_like(fd)?.set_nonblocking(arg & (O_NONBLOCK as usize) > 0)?;
    Ok(0)  // O_APPEND, O_DIRECT, O_NOATIME 全部丢弃
}
```

测试 `setfl_append_via_fcntl` 验证：`fcntl(fd, F_SETFL, O_APPEND)` 后写入数据仍覆盖文件开头而非追加，确认 O_APPEND 设置无效。

### C.4 copy_file_range 测试 (`tests/cases/test_copy_file_range.c`)

**总结：3 PASS / 4 FAIL（其中 1 个为测试自身 bug）— 发现数据损坏 bug**

| 测试 | 结果 | 说明 |
|------|------|------|
| `basic_copy_between_files` | **PASS** | 不同文件间基本复制正常 |
| `copy_with_offsets` | **FAIL** | 测试期望字符串错误（测试自身 bug，非内核 bug） |
| `flags_nonzero_should_fail` | **FAIL** | flags=1 被接受并返回 8 字节（Linux 要求 flags=0，否则 EINVAL） |
| `same_file_forward_overlap_corruption` | **FAIL** | **数据损坏确认**：byte 6096 得到 0x30，期望 0x00 |
| `copy_zero_length` | **PASS** | 0 长度复制正确返回 0 |
| `copy_past_eof` | **PASS** | 超过 EOF 的复制正确返回 0 |
| `pipe_should_fail` | **FAIL** | 管道作为输入被接受（Linux 要求常规文件，否则 EINVAL） |

**发现的 3 个 Bug**：

#### Bug #1（严重）：同文件前向重叠复制导致数据损坏

`do_send()` 使用 4096 字节缓冲区逐块读写（io.rs:263-292）。当 src 和 dst 是同一文件且前向重叠时：

```
文件: [0..8191]，模式 byte[i] = i & 0xFF
操作: copy_file_range(fd, &0, fd, &2000, 6000, 0)

Step 1: read file[0..4095] → buffer          ✓ 正确
Step 2: write buffer → file[2000..6095]       ← 覆写了 file[4096..6095]!
Step 3: read file[4096..5999] → buffer        ← 读到被覆写的数据
Step 4: write buffer → file[6096..7999]       ← 写入损坏数据

结果: file[6096] = 0x30 (来自 original[2096])
期望: file[6096] = 0x00 (来自 original[4096])
```

**修复方向**：检测同文件重叠场景，使用反向复制或临时缓冲区。

#### Bug #2：flags 参数被忽略

`_flags: u32` 参数完全未使用（io.rs:323）。Linux 要求 flags 必须为 0，任何非零值应返回 EINVAL。

#### Bug #3：无常规文件类型检查

`copy_file_range` 应仅接受常规文件。管道、套接字等特殊文件应返回 EINVAL。当前实现委托给 `do_send` 处理任意 `FileLike`。

### C.5 mmap 边界测试 (`tests/cases/test_mmap_edge.c`)

**总结：7 PASS / 1 FAIL — mmap 实现整体稳固**

| 测试 | 结果 | 说明 |
|------|------|------|
| `anon_private_basic` | **PASS** | 匿名私有映射：零填充、可写 |
| `anon_with_fd_positive` | **FAIL** | MAP_ANONYMOUS+fd=3 被拒绝（Linux 忽略 fd） |
| `file_backed_read` | **PASS** | 文件映射读取正常 |
| `length_zero_fails` | **PASS** | length=0 正确返回 EINVAL |
| `unaligned_offset_fails` | **PASS** | 非页对齐偏移正确返回 EINVAL |
| `private_cow_isolation` | **PASS** | MAP_PRIVATE COW 正确：写入不影响文件 |
| `fixed_noreplace_on_existing` | **PASS** | MAP_FIXED_NOREPLACE 正确返回 EEXIST |
| `shared_anon_mapping` | **PASS** | MAP_SHARED\|MAP_ANONYMOUS 正常工作 |

**发现的 1 个 Bug**：

#### MAP_ANONYMOUS 与正 fd 值不兼容

`mmap.rs:123`：

```rust
if map_flags.contains(MmapFlags::ANONYMOUS) != (fd <= 0) {
    return Err(AxError::InvalidInput);
}
```

当 `MAP_ANONYMOUS` 与 fd > 0 一起使用时，条件为 `true != false → EINVAL`。Linux 行为：MAP_ANONYMOUS 时 fd 被忽略，任何值都应被接受。

**修复**：改为 `if !map_flags.contains(MmapFlags::ANONYMOUS) && fd < 0 { return Err(InvalidInput); }`

**积极发现**：mmap 的 COW（写时复制）、FIXED_NOREPLACE、共享匿名映射、文件映射等核心功能均正确实现。

### C.6 memfd_create 测试 (`tests/cases/test_memfd_create.c`)

**总结：5 PASS / 2 FAIL — 基本功能正常但不是真正的匿名内存**

| 测试 | 结果 | 说明 |
|------|------|------|
| `basic_create_and_readwrite` | **PASS** | 创建、写入、读回正常 |
| `cloexec_flag` | **PASS** | MFD_CLOEXEC 正确设置 FD_CLOEXEC |
| `no_cloexec_by_default` | **PASS** | 默认不设置 FD_CLOEXEC |
| `anonymous_nlink_zero` | **FAIL** | st_nlink=1（真正 memfd 应为 0，无目录条目） |
| `ftruncate_and_mmap` | **PASS** | ftruncate 设置大小 + mmap 共享映射读写正常 |
| `data_not_persisted_after_close` | **FAIL** | 关闭后在 /tmp/memfd-* 找到数据（非匿名） |
| `independent_fds` | **PASS** | 多个 memfd 返回独立 fd |

**发现的 2 个 Bug**：

#### Bug #1：memfd 由真实文件支持而非匿名内存

`memfd.rs:17-28`：实现通过在 `/tmp/memfd-XXXX` 创建真实文件来模拟 memfd。`fstat` 显示 `st_nlink=1`（有目录条目），而真正的 Linux memfd 是匿名的（`st_nlink=0`，无可见路径）。

#### Bug #2：关闭后数据持久化

关闭 memfd fd 后，`/tmp/memfd-*` 文件仍然存在于文件系统中。真正的 memfd 在所有引用关闭后自动释放内存。这会导致 `/tmp` 目录下文件累积（最多 0xFFFF 个），最终导致 `sys_memfd_create` 返回 `TooManyOpenFiles`。

**积极发现**：基本的读写、cloexec、ftruncate、mmap 功能都正确。对于短生命周期的 memfd 使用场景，当前实现功能上可用。

### C.7 flock 测试 (`tests/cases/test_flock.c`)

**总结：4 PASS / 4 FAIL — flock 是完全的空操作**

| 测试 | 结果 | 说明 |
|------|------|------|
| `basic_exclusive_lock_unlock` | **PASS** | LOCK_EX + LOCK_UN 返回 0（stub 行为碰巧正确） |
| `basic_shared_lock_unlock` | **PASS** | LOCK_SH + LOCK_UN 返回 0 |
| `exclusive_blocks_exclusive` | **FAIL** | 第二个 LOCK_EX\|LOCK_NB 成功（应返回 EWOULDBLOCK） |
| `exclusive_blocks_shared` | **FAIL** | LOCK_EX 持有时 LOCK_SH 成功（应被阻止） |
| `shared_allows_shared` | **PASS** | 多个共享锁共存（碰巧正确，stub 都返回 0） |
| `shared_blocks_exclusive` | **FAIL** | LOCK_SH 持有时 LOCK_EX 成功（应返回 EWOULDBLOCK） |
| `unlock_allows_relock` | **PASS** | 解锁后重新加锁成功（碰巧正确） |
| `invalid_operation` | **FAIL** | flock(fd, 0) 成功（应返回 EINVAL） |

**确认的 Bug**：

`fd_ops.rs:308-312` 中 flock 的完整实现为：

```rust
pub fn sys_flock(fd: c_int, operation: c_int) -> AxResult<isize> {
    debug!("flock <= fd: {fd}, operation: {operation}");
    // TODO: flock
    Ok(0)
}
```

- 不验证 operation 参数（无效值也返回 0）
- 不维护任何锁状态
- 排他锁与共享锁之间无互斥
- 任何依赖文件锁正确性的程序（如数据库、包管理器）在 StarryOS 上会出现竞态条件

### C.8 测试覆盖总结

| Syscall | 测试数 | PASS | FAIL | 状态 | 发现 Bug 数 |
|---------|--------|------|------|------|------------|
| pwritev2 | 3 | 1 | 2 | buggy | 1 |
| mremap | 9 | 2 | 7 | broken | 5 |
| fcntl F_GETFL | 9 | 4 | 5 | buggy | 3+1 |
| copy_file_range | 7 | 3 | 4* | buggy | 3 |
| mmap | 8 | 7 | 1 | mostly ok | 1 |
| memfd_create | 7 | 5 | 2 | buggy | 2 |
| flock | 8 | 4 | 4 | stub | 1 (完全未实现) |
| **合计** | **51** | **26** | **25** | — | **~17 独立 Bug** |

*copy_file_range 的 4 个 FAIL 中 1 个为测试自身 bug

### C.9 待测 Syscall（按优先级排序）

| 排名 | Syscall | 理由 | 源码位置 |
|------|---------|------|----------|
| 1 | msgsnd/msgrcv | 阻塞发送/接收未实现 | ipc/msg.rs:482-610 |
| 2 | pipe2 | 未知标志仅警告不拒绝 | fs/pipe.rs:24-25 |
| 3 | eventfd2 | 可能有语义偏差 | — |
| 4 | dup3 (同 fd) | dup3(fd, fd, 0) 应返回 EINVAL | fs/fd_ops.rs:220 |
| 5 | clock_nanosleep | 忽略具体 clock 类型 | task/schedule.rs:28 |
