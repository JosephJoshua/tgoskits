# PostgreSQL on StarryOS: 内核适配日志

## 1. 目标

使 PostgreSQL 在 StarryOS 上完成三项操作:

1. **postmaster 启动** -- 进程进入主循环，包括后台工作进程 fork、共享内存初始化、Unix domain socket 监听
2. **initdb 建库** -- 创建数据目录、初始化 `pg_control`、写入系统目录表、设置文件权限
3. **基本查询** -- 通过 `psql` 连接，执行 `CREATE TABLE`, `INSERT`, `SELECT`

此目标不等同于通过 PostgreSQL 完整回归测试。PostgreSQL 回归测试套件包含两百余个测试文件，覆盖窗口函数、CTE、全文搜索、GiST 索引等大量特性。本研究的目标限于核心 SQL 链路的通路验证。

选择 PostgreSQL 作为适配目标，基于以下理由。在常见 Linux 应用中，数据库是少数同时深度依赖进程模型、进程间通信、文件系统语义、凭证系统、信号处理、事件循环的类别。Web 服务器（如 nginx）主要压力在 epoll 与 sendfile，键值存储（如 Redis）主要压力在 epoll 与内存分配。PostgreSQL 启动过程需依次经历: setresuid 切换用户身份、AF_UNIX socketpair 建立进程间通道、SysV shmget 分配共享内存、epoll 配合 self-pipe 构造事件循环、fsync 作用于目录文件描述符以确保元数据持久化、rename 与 fsync 组合实现原子持久化写入。上述每一项均对应独立的内核子系统，任一项未能正确处理，启动流程即中断。

## 2. PostgreSQL 进程模型与启动序列

### 2.1 进程模型

PostgreSQL 采用 Linux 经典的每连接独立进程模型（非线程池）。

```
postmaster (主进程)
  ├─→ startup process    (WAL 恢复, 一次性运行)
  ├─→ checkpointer       (脏页刷写)
  ├─→ bgwriter           (后台写入)
  ├─→ walwriter          (WAL 日志刷写)
  ├─→ autovacuum launcher (触发 autovacuum 工作进程)
  ├─→ archiver           (WAL 归档, 可选)
  ├─→ stats collector    (统计信息收集)
  └─→ backend process × N (每个客户端连接 fork 一个)
```

postmaster 是所有后台工作进程的共享父进程。每个后台工作进程在 `fork()` 后立即调用 `setresuid/setresgid` 切换到 postgres 用户身份，随后通过 `PR_SET_PDEATHSIG(SIGTERM)` 注册父进程终止信号。此为第一项关键依赖: 凭证切换与 PDEATHSIG 信号机制均须正确运作。

### 2.2 进程间通信层

PostgreSQL 使用三条独立的 IPC 通道。

**AF_UNIX socketpair。** postmaster 与后端进程之间通过 socketpair 传递文件描述符（SCM_RIGHTS）及状态消息。此通道是最核心的 IPC 路径: fork 后子进程继承 socketpair 的一端，postmaster 保留另一端，子进程通过该通道向 postmaster 报告就绪状态。

**SysV 共享内存。** `shared_buffers` 构成 PostgreSQL 的页缓存层，所有后端进程共享同一块 SysV 共享内存。启动时 postmaster 调用 `shmget(key, size, IPC_CREAT | 0600)` 创建内存段，继而通过 `shmat` 将之映射至自身地址空间。fork 产生的子进程继承此映射。此为第二项关键依赖: SysV 共享内存须正确支持多进程并发 attach/detach，且在 SMP 条件下不可出现死锁。

**Self-pipe 机制。** PostgreSQL 的事件循环采用经典 self-pipe 模式: 创建管道，将读端加入 epoll 监听集合，需唤醒 `epoll_wait` 时向写端写入单字节。PostgreSQL 将此机制包装为 WaitEventSet，并对其中的 latch 文件描述符反复执行 `EPOLL_CTL_MOD` 以重新武装。epoll 的 CTL_MOD 语义与电平触发模式的行为是关键依赖。

### 2.3 存储层

PostgreSQL 的持久化写入采用 durable_rename 模式:

```
write(tmp_file)          -- 将数据写入临时文件
fsync(tmp_file)          -- 确保数据到达存储介质
rename(tmp_file, final)  -- 原子重命名为最终路径
fsync(parent_dir)        -- 确保目录项持久化 (POSIX 要求)
```

此模式依赖三项内核行为: fsync 作用于目录文件描述符时不可返回 EISDIR；rename 执行后目标文件内容不可丢失（涉及 VFS 的 DirEntry 及 page cache 生命周期管理）；rename 操作须保持原子性。PostgreSQL 启动时还对数据目录调用 `fsync(data_dir_fd)` 以确保元数据持久化，这是 initdb 路径上的首个阻塞点。

### 2.4 完整启动序列

以下展开 PostgreSQL 启动过程中每一步所涉及的内核接口:

```
 0. postgres 用户执行 pg_ctl start
 1. postmaster fork 第一个子进程
 2. 子进程 setresuid/setresgid → uid=70 (postgres)
 3. socketpair(AF_UNIX, SOCK_STREAM) → 建立 IPC 通道
 4. bind(AF_UNIX, "/tmp/.s.PGSQL.5432") → 创建监听 socket
 5. chmod("/tmp/.s.PGSQL.5432", 0777) → 设置权限
 6. listen(sock, SOMAXCONN) → 开始监听
 7. shmget(IPC_PRIVATE, shared_buffers_size, IPC_CREAT|0600)
 8. shmat(shmid, NULL, 0) → 多次 attach (SMP 并发)
 9. epoll_create1(EPOLL_CLOEXEC) → 创建事件循环实例
10. pipe2(self_pipe, O_NONBLOCK) → 创建 self-pipe
11. epoll_ctl(epfd, EPOLL_CTL_ADD, self_pipe[0], ...)
12. epoll_ctl(epfd, EPOLL_CTL_MOD, latch_fd, ...) → WaitEventSet 重武装
13. epoll_wait(epfd, events, maxevents, -1)
14. fork() → 后台工作进程
15. 子进程 prctl(PR_SET_PDEATHSIG, SIGTERM)
16. 子进程 prlimit64(RLIMIT_NOFILE, ...) → 提高文件描述符限制
17. 子进程 getrlimit(RLIMIT_STACK) → 计算 max_stack_depth
18. fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) → memfd 缓冲区共享 (可选)
```

随后进入 initdb:

```
19. open(data_dir, O_RDONLY) → 获取目录文件描述符
20. fsync(fd) → 刷新目录元数据          ← StarryOS 返回 EISDIR
21. write(pg_control_tmp, data)
22. fsync(pg_control_tmp)
23. rename(pg_control_tmp, pg_control) → durable_rename  ← 内容清零
24. fsync(data_dir_fd)
25. fchown(pg_control, uid=70, gid=70) → 设置文件属主
26. fchmod(pg_control, 0600) → 设置文件权限
```

每一步标注了 StarryOS 上曾经发生中断的位置。下文按发现时序逐一记录每个阻塞点的修复过程。

## 3. 修复记录

### 3.1 #197 -- preadv/pwritev 系统调用 ABI 修正

**日期**: 2026-04-15
**链接**: https://github.com/rcore-os/tgoskits/pull/197

此为整个 PostgreSQL 适配工作的起点。

在 StarryOS 上执行最基本的 C 程序即可观察到异常: `pwritev` 返回成功但数据未被写入，`preadv2` 的 flags 参数在每次调用时呈现不同数值。调试结果表明存在两个独立问题。

其一，`sys_pwritev2` 的实现自 `sys_preadv2` 复制而来，但其内部调用仍为 `.read_at()`。数据经 VFS 层传递后被当作读操作处理，写入内容全部丢失。此缺陷表明此前无人对 `pwritev` 进行过端到端测试。常规应用倾向于使用 `write` 或 `pwrite`，向量 I/O 的使用频率较低。

其二，更为隐蔽。preadv/pwritev/preadv2/pwritev2 四个系统调用的参数数量与 Linux 内核不一致。Linux 内核中 preadv/pwritev 定义为 `SYSCALL_DEFINE5`(fd, vec, vlen, pos_l, pos_h)，preadv2/pwritev2 定义为 `SYSCALL_DEFINE6`(fd, vec, vlen, pos_l, pos_h, flags)。StarryOS 此前 preadv/pwritev 仅读取 4 个参数（遗漏 pos_h），preadv2/pwritev2 仅读取 5 个参数（遗漏 flags）。遗漏的 pos_h 和 flags 读到的实为用户态栈上的残留值。

值得注意的细节: 64 位偏移量在 64 位架构上由单一寄存器传递（loongarch64 及 riscv64 的 64 位 ABI），但在 32 位兼容模式或特定 64 位 ABI 约定下需拆分为两个寄存器（pos_l 与 pos_h）。StarryOS 的系统调用分发层自早期简化实现逐步演进，参数数量的变化在此过程中被遗漏。

修复方案直接但具有信号意义: 它表明 StarryOS 存在从内核其他位置复制粘贴系统调用实现而未对照 Linux 源码逐一核验 ABI 签名的情况。此模式在后续工作中反复出现。

**反思**: 每个系统调用的参数签名均应参照 Linux 的 `SYSCALL_DEFINEn` 宏逐一核对。从其他实现复制粘贴是缺陷的温床。此项检查可以且应当自动化。

### 3.2 #246 -- 进程凭证子系统

**日期**: 2026-04-18
**链接**: https://github.com/rcore-os/tgoskits/pull/246

此 PR 是整个 PostgreSQL 适配过程中工程量最大的单项修改。PostgreSQL 的 initdb 首步操作即为 `setresuid(uid=70, euid=70, suid=70)` 切换至 postgres 用户身份，随后通过 `fchown(pg_control, uid=70, gid=70)` 设置文件属主。StarryOS 此前将这两类调用实现为空操作: `getuid()` 恒返回 0，`setresuid()` 不产生任何效果，`chown/fchown` 不进行权限检查。

Linux 的 `struct cred` 包含八个身份字段及补充组列表。`uid`/`gid` 为真实身份，标识进程的初始启动者。`euid`/`egid` 为有效身份，用于绝大多数权限检查。`suid`/`sgid` 为保存的设置用户 ID，允许进程在特权与非特权身份之间切换（例如，一个 setuid 程序可在需要时恢复为原始 uid）。`fsuid`/`fsgid` 为文件系统访问专用身份，通常情况下与 euid/egid 一致，但可通过 `setfsuid(2)` 独立修改，此设计是 NFS 服务器模拟客户端身份的基础。补充组列表通过 `setgroups(2)` 管理。

`setresuid` 的一项容易遗漏的语义为 NOCHG: 传入 -1 表示该字段不修改。`setresuid(-1, 1000, -1)` 的语义为将 euid 设为 1000，uid 与 suid 保持不变。Linux man page 明确记载此行为，但多数从头实现的凭证系统会错失此细节。

StarryOS 的实现采用 `Arc<Cred>` 引用计数共享，`Thread` 中存放 `SpinNoIrq<Arc<Cred>>`。fork 时子进程继承父进程的 Arc。权限检查须接入所有接触点: `faccessat2`（基于 fsuid 而非 euid 检查）、`fchownat`（需 CAP_CHOWN 或文件属于调用者）、`fchmodat`（需 CAP_FOWNER 或文件属于调用者）、`kill/tkill/tgkill`（发送者的 euid/uid 须匹配目标进程的 uid/euid/suid 之一）、IPC 子系统的 msgget/msgsnd/msgrcv/shmget、`/proc/<pid>/status` 的 Uid/Gid 字段。

一项设计决策为能力检查暂时以 `euid==0` 近似。标准 Linux 使用 capabilities 位掩码（`CAP_SYS_RESOURCE`, `CAP_CHOWN`, `CAP_KILL` 等），StarryOS 尚未实现 capabilities 子系统，故以 root 身份（euid==0）作为过渡方案。在工作站场景下此特化合理，但长期须替换为完整的能力检查。

**反思**: 凭证系统属于内核中基础性最强的组件。PostgreSQL 在此处的暴露最为彻底: 整个进程模型建构于缺失的安全基础之上。缺乏凭证系统时，`setresuid` 无实际效果，`chmod/chown` 无实际效果，IPC key 的权限隔离不复存在，任意进程可向任意其他进程发送信号。

### 3.3 #247 -- SA_RESTART 系统调用重启语义

**日期**: 2026-04-18
**链接**: https://github.com/rcore-os/tgoskits/pull/247

PostgreSQL 的 postmaster 在 `accept()` 上阻塞等待客户端连接。收到信号（例如 SIGUSR1 通知有工作进程退出需回收）后，信号处理函数执行完毕，`accept()` 应自动重启并继续等待。

SA_RESTART 是 `sigaction(2)` 的一个标志位，设置后，被信号中断的阻塞系统调用将自动重启。然而"重启"在内核层面涉及若干细节。

在 RISC-V 与 AArch64 架构上，系统调用的首参数寄存器与返回值寄存器为同一物理寄存器: RISC-V 为 `a0`，AArch64 为 `x0`。系统调用进入内核后，内核完成实际操作，将返回值写入 `a0`。正常系统调用中该值为正数（如 `read` 返回读取字节数）。信号中断时，内核将 `-EINTR` 写入 `a0` 表示调用被中断。

但若设置了 SA_RESTART，内核不应返回 EINTR。它应以透明方式重新执行该系统调用。此操作要求同时满足三个条件:

1. PC 须回退至 syscall 指令自身，而非 syscall 之后。RISC-V 的 `ecall` 指令为 4 字节，AArch64 的 `svc` 同为 4 字节，故 PC -= 4。x86_64 的 `syscall` 指令为 2 字节，PC -= 2。

2. `a0` 须恢复为系统调用的原始首参数。中断时 `a0` 已被 `-EINTR` 覆盖。若重启时不恢复，系统调用将以 fd=0xFFFFFFFFFFFFFFEC 作为参数执行，必然失败。

3. 不可调用 `set_retval(0)` 清除 EINTR。在 RISC-V/AArch64 上，`set_retval` 与 `set_arg0` 写入同一寄存器。先恢复 `a0` 再 `set_retval(0)`，`a0` 再次变为 0。

上述三个条件在 x86_64 上不受影响，因 x86_64 的 `rdi`（arg0）与 `rax`（retval）为不同寄存器。但不能因此仅在 RISC-V/AArch64 上修复。实现须对所有架构保持一致。

StarryOS 此前既不保存原始 `a0` 值，`check_signals` 函数亦不接受任何与重启相关的参数。修复引入 `SyscallRestartInfo { saved_a0: usize }`，在进入 `handle_syscall` 之前保存，传递至信号处理路径。检测到 SA_RESTART 且返回值为 EINTR 时，执行 PC 回退与 a0 恢复。`rt_sigtimedwait` 与 `rt_sigsuspend` 两个系统调用自身即期望 EINTR 语义，不应参与重启，此二调用点传入 `None` 作为 restart_info。

**反思**: SA_RESTART 表面是一个简单的标志位，实现它需要理解三个层次: POSIX 层面的语义（何种情况下应重启，何种情况下不应重启），内核层面的实现机制（信号栈帧、返回值传递、PC 操作），架构 ABI 层面的细节（寄存器复用、指令长度、set_retval 与 set_arg0 的冲突）。逐层拆解后可见，一个 SA_RESTART 标志位背后包含了对操作系统如何管理用户态上下文的完整理解。

### 3.4 #249 -- PR_SET_PDEATHSIG 父进程死亡信号

**日期**: 2026-04-18
**链接**: https://github.com/rcore-os/tgoskits/pull/249

PostgreSQL 的后台工作进程（checkpointer, bgwriter, walwriter 等）在 `fork()` 之后立即执行:

```c
prctl(PR_SET_PDEATHSIG, SIGTERM);
```

语义为: 若父进程终止，向本进程发送 SIGTERM。此乃 postmaster 崩溃时自动清理全部工作进程的机制。若缺失此项，postmaster 终止后工作进程继续运行，持有共享内存段不释放，端口持续被占用。

StarryOS 此前 `sys_prctl` 中 `PR_SET_PDEATHSIG`/`PR_GET_PDEATHSIG` 两个命令无对应的 match arm，落入默认分支被静默忽略。存在三处缺失: 内核无存储 pdeathsig 值的字段（对应 Linux `task_struct.pdeath_signal`），进程退出时无向子进程投递信号的逻辑（对应 Linux `do_exit` → `forget_original_parent` 路径），`sys_prctl` 无分发入口。

修复在 `Thread` 中增设 `pdeathsig: AtomicU32` 字段，`sys_prctl` 增加两条 match arm（SET 校验信号编号不超过 64，GET 读回当前值），`do_exit` 中遍历全部子进程读取 pdeathsig 并投递信号。测试采用三层进程结构（祖父→父→子），父进程退出后子进程通过管道通知祖父进程已收到 SIGUSR1。

**反思**: 此功能工程量不大，但它揭示了一种模式: PostgreSQL 每个后台工作进程的 fork 之后均有一段初始化代码（setresuid, PR_SET_DEATHSIG, prlimit64, getrlimit）。任一步骤失效均导致工作进程或 postmaster 异常。这些初始化代码并非 PostgreSQL 的特殊需求，它们是 Linux 进程管理的基础设施。

### 3.5 #248 -- RLIMIT_STACK 默认值修正

**日期**: 2026-04-18
**链接**: https://github.com/rcore-os/tgoskits/pull/248

PostgreSQL 启动时调用 `getrlimit(RLIMIT_STACK, &rlim)` 读取栈大小限制，以此计算 `max_stack_depth`。`max_stack_depth` 用于限制递归查询深度，防止递归导致栈溢出。

StarryOS 此前 RLIMIT_STACK 软限制的默认值为 `USER_STACK_SIZE`（512 KiB），即内核为用户态栈做初始映射的大小。Linux 默认值为 8 MiB（`_STK_LIM`）。512 KiB 的默认值致使 PostgreSQL 计算得到的 `max_stack_depth` 较 Linux 上小 16 倍，部分正常递归查询提前报错。

修复本身为一行数值修改（512 KiB 改为 8 MiB）。需注意 PR 标题所述"DTB 内存解析"在实际实现中被划除: PR 正文中 DTB 相关段落使用了删除线标记。DTB 部分未交付，仅修改了栈限制。标题遗留 DTB 字样属历史原因（初始计划同时完成两项），实际交付内容仅涉及栈限制。

**反思**: PR 标题与实际内容不一致是一个需注意的流程问题。此问题发生在开发早期（前五日内），流程纪律尚在建立过程中。后续 PR 的标题与内容一致性有明显改善。

### 3.6 #251 -- fsync 对目录文件描述符的处理及 sync_file_range 实现

**日期**: 2026-04-18
**链接**: https://github.com/rcore-os/tgoskits/pull/251

initdb 在创建数据目录及 `pg_control` 文件后，调用 `fsync(data_dir_fd)` 确保目录元数据持久化。StarryOS 返回 `EISDIR`，即"目标为目录"。

Linux 内核允许对目录文件描述符调用 `fsync`。`fs/sync.c` 的 `vfs_fsync_range` 对目录类型 inode 调用文件系统自身的 `fsync` 方法；若文件系统不支持目录级同步（如 tmpfs），则静默返回 0。POSIX 将此行为列为扩展，SQLite 与 PostgreSQL 均重度依赖该扩展。

StarryOS 的错误来自 `File::from_fd` 对目录文件描述符返回 `AxError::IsADirectory`，`sys_fsync` 未处理此错误即向上传递。修复在此错误类型上做特殊处理，返回 `Ok(0)`。

同步实现了 `sync_file_range(2)`，此前该调用直接返回 ENOSYS。PostgreSQL 的 WAL 写入路径使用此调用进行预刷写。`sync_file_range(2)` 的 man page 明确指出该调用不提供数据完整性保证，内核可合法将其实现为无操作。StarryOS 将其委托给 `f.inner().sync(true)`（datasync），对内存文件系统为无操作，对块设备文件系统触发数据刷写。

**反思**: fsync 作用于目录这一行为，在接触此问题之前本人并不知晓 Linux 支持此操作。POSIX 标准仅规定了 fsync 对普通文件的行为，目录 fsync 是 Linux 扩展。但 PostgreSQL 使用了它。"标准未记载但 Linux 实现且应用依赖"的扩展是兼容性工作中占比最大的类别。

### 3.7 #250 -- epoll_pwait sigsetsize 兼容性

**日期**: 2026-04-18
**链接**: https://github.com/rcore-os/tgoskits/pull/250

以 musl libc 编译的 PostgreSQL 调用 `epoll_pwait` 时收到 `EINVAL`。musl 的 `epoll_pwait` 实现传入的 `sigsetsize` 为 `_NSIG/8 = 16` 字节（musl 的 `sigset_t` 为 128 位），而 StarryOS 的 `check_sigset_size` 仅接受内核 `SignalSet` 的大小（8 字节）或 0。收到 16 时直接返回 EINVAL。

Linux 内核接受任意 `sigsetsize >= sizeof(kernel_sigset_t)` 的值，仅使用低 64 位。musl 传入 16，glibc 传入 8。StarryOS 的严格等于检查仅考虑了 glibc 的情况。

同时，原 `do_epoll_wait` 在 `sigmask` 指针为空时仍调用 `check_sigset_size`。`epoll_wait` 不传递 sigmask（sigmask=NULL），却受到此项无意义的检查阻碍。

修复方案: `check_sigset_size` 接受 0、8、16 三种值；`do_epoll_wait` 仅在 `!sigmask.is_null()` 时才进行检查。

**反思**: 此问题本质为过度严格的输入验证。ABI 检查应遵循宁松勿紧原则。拒绝一个合法输入比接受一个非法输入更难排查。musl 与 glibc 在此参数上的不一致是 C 库生态的历史债务，内核的职责是兼容两者的合法输入。

### 3.8 #311 -- AF_UNIX SOCK_STREAM 末端语义

**日期**: 2026-04-24
**链接**: https://github.com/rcore-os/tgoskits/pull/311

PostgreSQL 的后台工作进程与 postmaster 通过 AF_UNIX socketpair 通信。工作进程完成初始化后关闭写端，postmaster 在另一端读取完全部数据后调用 `recv()`。Linux 返回 0 表示 EOF，StarryOS 返回 `EAGAIN`。

`EAGAIN` 的语义为当前无数据但后续可能有数据到达。调用方看到 EAGAIN 应继续循环等待。但写端已关闭，不再可能有新数据到达。postmaster 因此永久阻塞。

根因位于 `StreamTransport::recv` 自 ring buffer 读取零字节时直接返回 `WouldBlock`，未先判断写端是否仍然存活。修复增加一行 `chan.rx.write_is_held()`: 写端已释放则返回 `Ok(0)`（EOF），写端仍存活则返回 `WouldBlock`。

**反思**: 一行修复，语义完全改变。`EAGAIN` 与 EOF（返回 0）的差异本质上是"暂时无数据，继续等待"与"传输结束，停止等待"的差异。此为网络协议设计中的核心概念: 关闭连接与暂时空转是两种截然不同的状态。

### 3.9 #313 -- AF_UNIX socket 文件属主修正

**日期**: 2026-04-30
**链接**: https://github.com/rcore-os/tgoskits/pull/313

PostgreSQL 的 postmaster 以 uid=70（postgres 用户）运行，`bind(AF_UNIX, "/tmp/.s.PGSQL.5432")` 创建 socket 文件后调用 `chmod(0777)` 设置权限。

StarryOS 上 `chmod` 返回 `EPERM`。根因: `bind` 创建 socket 文件时未携带调用者的 credential 上下文，inode 属主始终为 uid=0 gid=0。非 root 进程调用 `fchmodat` 时，VFS 检查 fsuid 与 inode uid 不匹配，返回 EPERM。

Linux 的行为是 `bind()` 创建的 socket 文件属主取当前任务的 fsuid 与 fsgid。修复在 `sys_bind` 成功后，对 AF_UNIX pathname 路径额外执行一步 chown: 通过 FS_CONTEXT 解析 socket 文件路径，调用 `update_metadata` 将 uid/gid 改为当前线程 credential 的 fsuid/fsgid。

此修复依赖 #246 的凭证子系统。缺乏 #246 则无 fsuid/fsgid 可用，无法正确设置属主。

**反思**: #313 与 #246 存在依赖关系。#246 提供凭证基础设施，#313 在 socket 创建路径上使用该基础设施设置属主。若两者合入顺序颠倒，#313 无法工作。实际合入顺序为 #246 先合（04-18），#313 后合（04-30），间隔 12 天。此间隔主要因 #246 规模较大（修改涉及 12 个文件），审查耗时超出预期。

### 3.10 #312 -- VFS rename 操作中 DirEntry 的保持

**日期**: 2026-04-24
**链接**: https://github.com/rcore-os/tgoskits/pull/312

PostgreSQL 的 initdb 创建 `pg_control` 等文件后，读取结果全为零字节。SQL 层面报 "replication checkpoint has wrong magic 0"，拒绝启动。

PostgreSQL 的写入流程使用了前述 durable_rename 模式。问题发生在 rename 步骤。

StarryOS 的 `DirNode::rename` 在底层 rename 成功后，对源路径与目标路径分别调用了 `forget_entry`。对源路径调用 `forget_entry` 意味着将源 DirEntry 自目录缓存中移除。tmpfs 无持久存储，文件内容仅存在于 DirEntry 的 `user_data` 字段通过 `Arc` 持有的 page cache 中。DirEntry 移除后 Arc 引用计数归零，page cache 被释放。随后按目标文件名执行 lookup，分配到全新的空 page cache。rename 前写入的所有字节丢失。

修复将源 DirEntry 自源目录取出（非 forget），以目标文件名插入目标目录。DirEntry 实例保持不变，挂载其上的 page cache 得以延续，内容完整保留。目标路径的旧 entry 仍执行 forget（因其已被替换）。

**反思**: rename 的语义在 POSIX 中有明确定义但在内核实现中容易出现偏差。rename 是将同一文件对象重关联至新名称，而非将内容复制至新路径后删除旧路径。对 tmpfs 此类纯内存文件系统，维持此语义的唯一途径是保持 DirEntry 实例不变。DirEntry 一旦变更，page cache 即断裂。

### 3.11 #314 -- EPOLL_CTL_MOD 后事件保持

**日期**: 2026-04-24
**链接**: https://github.com/rcore-os/tgoskits/pull/314

PostgreSQL 的 WaitEventSet 对 latch 文件描述符反复执行 `EPOLL_CTL_MOD` 重新武装。MOD 执行后，`epoll_wait` 收不到该文件描述符的事件，持续阻塞直至 PostgreSQL 自身 60 秒超时。

根因为 `Epoll::modify` 的操作序列: 将 interests 哈希表中的旧 `Arc<EpollInterest>` 替换为新对象。旧 Arc 引用计数归零后被释放。但 `ready_queue` 中仍悬挂指向旧 Arc 的 `Weak<EpollInterest>`。`poll_events()` 遍历 `ready_queue`，对每个 Weak 调用 `upgrade()`。旧 Arc 已不存在，`upgrade()` 返回 None 被跳过。此条就绪事件彻底消失。

新创建的 Arc 无任何路径进入 `ready_queue`。epoll 文件描述符自身的 waker 未被触发，因此亦无新事件重新入队。仅能等待底层文件描述符的下一次真实事件（例如新数据到达管道），waker 再次触发，新 Interest 才入队。PostgreSQL 无法等到此条件，连接超时。

修复方案: 替换 interests 表之前，记录旧 Arc 的 `is_in_queue()` 状态。若旧条目已在 ready_queue 中，释放锁后立即将新 Arc 的 Weak 推入 ready_queue，标记 `in_ready_queue=true`。旧 Weak 保留在队列中无影响，`upgrade()` 失败分支会跳过。

**反思**: `Arc` 与 `Weak` 的组合在并发数据结构中存在一个经典陷阱: Weak 指向的对象被释放后，队列中悬挂的 Weak 数量未知，它们占据队列位置但每次 upgrade 均返回 None。epoll 的此缺陷本质上是对象生命周期管理问题，非并发问题。修复思路亦为经典模式: 在旧对象被替换之前，将新对象的指针放入旧对象所在的所有队列中。

### 3.12 #319 -- prlimit64 静默失效

**日期**: 2026-04-24
**链接**: https://github.com/rcore-os/tgoskits/pull/319

PostgreSQL 的 initdb 通过 `prlimit64` 提高 `RLIMIT_NOFILE` 的硬限制，以便打开更多文件描述符。调用返回成功（ret=0），但限制未改变。后续 `open` 因超过软限制而失败。

StarryOS 原实现逻辑:

```rust
if new_limit.rlim_max <= limit.max {
    limit.max = new_limit.rlim_max;
} else {
    // TODO: patch resources
    return Ok(0);
}
```

提高硬限制（`new_limit.rlim_max > limit.max`）进入 else 分支，返回 `Ok(0)` 但不修改 `limit.max`。调用方看到返回 0 以为限制已提高，实际未生效。此为实现中最差的情形: 比返回 `EPERM` 更难排查，因调用方没有理由怀疑"成功但未生效"。

`prlimit(2)` 的语义: 无特权进程仅可降低硬限制（不可逆）；持有 `CAP_SYS_RESOURCE` 的进程可任意提高。StarryOS 以 root 运行，正确行为是允许提高并使之生效。

修复直接执行 `limit.max = new_limit.rlim_max`，并添加 TODO 注释: 待 capabilities 子系统实现后，对无 CAP_SYS_RESOURCE 的提高操作返回 EPERM。

**反思**: 此缺陷体现了静默失效模式。内核接受了参数，返回了成功，但未产生任何副作用。从 API 设计的角度，一次返回成功的调用必须产生其承诺的副作用。若合法地无法执行任何操作（如权限不足），则应返回错误码，使调用方有机会处理。

### 3.13 #226 -- IPC 共享内存死锁

**日期**: 2026-04-21
**链接**: https://github.com/rcore-os/tgoskits/pull/226

PostgreSQL 启动 shared buffer（SysV 共享内存）时，SMP=4 的 QEMU 环境下几乎必然死锁。20 次测试中 19 次超时。

`sys_shmget` 的加锁顺序: SHM_MANAGER → shm_inner。`sys_shmat` 的加锁顺序: shm_inner → aspace → SHM_MANAGER。经典 AB/BA 死锁。

修复未采用增大锁粒度的方案。`sys_shmat` 改为按 SHM_MANAGER → shm_inner → aspace 顺序一次性获取三把锁。`sys_shmdt` 采用分阶段加锁: 先查 shmid（持有 SHM_MANAGER 后释放），读 va_range（持有 shm_inner 后释放），解除映射（仅持有 aspace），最后更新 bookkeeping（重新获取 SHM_MANAGER 与 shm_inner）。全局锁顺序统一为 SHM_MANAGER → shm_inner → aspace。

同步修复了三个附带问题: `clear_proc_shm` 缺失 `aspace.unmap()` 导致页表项残留（物理页已释放但映射仍在，构成 use-after-free）；`IPC_RMID` 在 nattch==0 时未立即销毁内存段；`attach_process/detach_process` 使用 `assert!` 而非返回错误，恶意用户态程序可触发内核 panic。

**反思**: 锁顺序问题是 SMP 环境下的系统性风险。不执行 SMP 测试时此类缺陷不会暴露。PostgreSQL 恰为能在 SMP 条件下密集调用 shmget+shmat 的应用（并行启动多个 backend 进程），故为理想的测试负载。"分阶段加锁"是一种通用重构模式: 将一次性持有大锁从头至尾改为多个阶段，每阶段仅持有必要的锁并尽快释放。代码会因此变得更复杂（须考虑中间状态被其他线程修改的情况），但死锁风险显著降低。

### 3.14 #316 -- interrupt waker 竞态条件

**日期**: 2026-04-24
**链接**: https://github.com/rcore-os/tgoskits/pull/316

抢占式调度下，`TaskInner::poll_interrupt` 存在一个窗口: `interrupt()` 的唤醒信号可能丢失，被 await 的任务永远停留在等待状态。PostgreSQL 的并行查询工作进程在等待父进程信号时依赖此机制。

原实现先执行 `interrupted.swap(false, AcqRel)`，仅在观察到 false 时才注册 waker。在 swap 与 register 之间，若时钟中断触发调度，另一任务调用 `interrupt()` 发起唤醒。Interrupt() 发现 `interrupt_waker` 槽仍为空，`wake()` 找不到目标被丢弃。随后 `poll_interrupt` 挂上 waker，但 `interrupted` 标志已被清零。再无任何实体触发此 waker，任务永久挂起。

修复与 #247 的 SA_RESTART、b17 的 waitpid 遵循同一模式: register-then-check。先将 `interrupt_waker.register(cx.waker())` 移至 `interrupted.swap(false, AcqRel)` 之前。若 swap 观测到 true，说明注册前已有中断到达，直接返回 Poll::Ready。若观测到 false，waker 已就位，后续任何 `interrupt()` 调用均能找到它。

**反思**: register-then-check 模式在本次项目中反复出现: 信号路径（b15）、waitpid（b17）、interrupt waker（#316）、epoll 的 `check_and_register_waker`（#504）。其核心思想为: 先建立通知渠道，再检查是否有待处理事件。若事件在渠道建立之前到达，渠道建立后立即可见。若顺序相反（先检查再注册），则永远存在一个窗口: 事件在检查之后、注册之前到达，渠道尚未建立，事件丢失。

### 3.15 #504 -- epoll 电平触发模式就绪队列修正

**日期**: 2026-05-12
**链接**: https://github.com/rcore-os/tgoskits/pull/504

电平触发模式下 `epoll_wait(maxevents=N)` 对单个就绪文件描述符返回 N 份完全相同的 `epoll_event`。此缺陷与 #314 互补，共同封堵了 epoll LT 路径上的回归点。

根因为一循环结构: `pop_front` 取出 `interest` 写入 `out[count]`，随后同一 `Weak<EpollInterest>` 被 `push_back` 回 `ready_queue`。下一轮 `pop_front` 取出的仍是同一 interest，再次消费，再次推回。直至 `count` 填满 `out`（等于 `maxevents`）。一次就绪事件被报告为 `maxevents` 份副本。

Linux `fs/eventpoll.c` 的 `ep_send_events` 采用完全不同的模型: 将整个 `ready_queue` 一次性排空至 `txlist`，每个 interest 仅访问一次，电平触发模式下的 interest 放入 `keep` 队列，循环结束后整体 splice 返回。

修复对齐该模型: 使用 `mem::take` 一次性排空 `ready_queue` 至本地 `txlist`，每个 interest 仅访问一次，LT 留存的放入 `keep` 队列，循环结束整体 splice 回 `ready_queue` 并执行 `wake()`。并发 wake 路径仅向 `ready_queue` 写入，不与 `txlist` 冲突。

同步修复了一个并发窗口: `consume()` 检查与 `register()` 之间到达的事件会落入旧 waker，旧 waker 观测到 `in_ready_queue=true` 即放弃处理（默认已有其他处理者在工作），但此为假象。修复采用 `check_and_register_waker` 模式: 注册新 waker 后再次读取 `file.poll()`，若匹配到新事件立即 wake。

**反思**: 此 PR 与 #314 构成 epoll 实现的两个互补修复。#314 解决 MOD 后旧 Arc 的 Weak 被丢弃的问题，#504 解决 LT 模式下同一 Interest 被循环消费的问题。两者均涉及对象生命周期管理，均涉及就绪队列的数据结构设计。epoll 的 Linux 实现历经十余年迭代，StarryOS 初始简化实现将这些边界条件全部跳过。

### 3.16 #505 -- 用户态崩溃寄存器转储

**日期**: 2026-05-16
**链接**: https://github.com/rcore-os/tgoskits/pull/505

PostgreSQL 在 StarryOS 上崩溃时，内核仅输出一行 `"segmentation fault at addr"` 附加一个地址。调用栈与寄存器信息完全缺失。

此 PR 增加调试基础设施。`dump_user_crash_context(&UserContext)` 按四个架构分别输出关键寄存器: riscv64 输出 `sepc/ra/sp/gp/tp/s0/a0..a7`，aarch64 输出 `elr/spsr/x0..x3/x29/x30`，x86_64 输出 `rip/rsp/rflags/rax/rdi/rsi/rdx`，loongarch64 输出 `era/ra/sp/tp/a0..a3`。

挂载点置于实际会终结进程的两条路径上: `check_signals` 处理 `SignalOSAction::Terminate` 和 `CoreDump`；`raise_signal_fatal` 的兜底分支。不在 `SA_*` handler 拦截等未构成致命情形的路径上输出。输出使用 `warn!` 级别，QEMU 串口默认日志阈值为 Warn，无需调整 `RUST_LOG` 即可直接观测。

**反思**: 此 PR 工程量不大但实用价值显著。内核开发中最耗时的环节是定位缺陷位置，而非修复缺陷本身。PostgreSQL 发生段错误时，`sepc` 指示崩溃发生的指令地址，`ra` 指示调用者地址，`a0..a7` 指示当时的参数值。结合这些信息与 objdump 输出即可定位到源代码行。此前仅能依靠猜测。

## 4. 回顾

### 4.1 时序

```
04-15  #197  preadv/pwritev ABI          ← 修复最基础的 I/O 系统调用
04-18  #246  凭证子系统                   ← 安全基础设施
04-18  #247  SA_RESTART                  ← 信号语义
04-18  #248  RLIMIT_STACK 默认值         ← 资源限制
04-18  #249  PR_SET_PDEATHSIG            ← 进程管理
04-18  #250  epoll_pwait sigsetsize      ← epoll 兼容性
04-18  #251  fsync 目录 + sync_file_range ← 文件系统语义
04-21  #226  IPC 共享内存死锁             ← 并发正确性
04-24  #311  AF_UNIX EOF                 ← 网络语义
04-24  #312  VFS rename 原子性           ← VFS 正确性
04-24  #314  EPOLL_CTL_MOD 事件丢失      ← epoll 正确性
04-24  #319  prlimit64 静默无效          ← 资源限制
04-24  #316  interrupt waker 竞态        ← 并发正确性
04-30  #313  AF_UNIX chown               ← 凭证 + 网络
05-12  #504  epoll LT 重复事件            ← epoll 正确性
05-16  #505  用户态 crash dump            ← 调试基础设施
```

4 月 15 日至 18 日是产出最高的四日，合入 7 个 PR。此阶段基本上在系统性扫描 PostgreSQL initdb 的启动路径，每遇到一个阻塞点即修复、编写回归测试、提交。4 月 21 日至 30 日处理的缺陷更深层，涉及并发语义，不像 fsync 返回 EISDIR 那样立即暴露，需要 SMP 压力测试和反复验证才能稳定复现。

### 4.2 缺陷分层

PostgreSQL 适配过程中暴露的 StarryOS 问题可分为三个层次。

**缺失的功能**（#246）: 凭证子系统。整套安全模型须从头构建。

**语义偏差**（绝大多数 PR）: 系统调用存在，参数校验存在，基本路径可走通。但某个标志位组合、某个 errno 条件、某个边界情况与 Linux 不一致。具体而言: fsync 对目录文件描述符返回 EISDIR（#251），rename 后内容丢失（#312），epoll LT 返回重复事件（#504），prlimit64 返回成功但不生效（#319）。

**并发正确性**（#226, #316, #504）: 单核行为正确，SMP 条件下暴露竞态或死锁。此类缺陷须 SMP 压力测试方可暴露，PostgreSQL 的多进程并发启动过程是有效的触发条件。

### 4.3 后续工作

PostgreSQL 适配工作停止在"基本 SQL 可执行"的阶段。以下若干方向须继续推进但未在本周期内完成:

autovacuum 机制。autovacuum 工作进程需定期扫描表、更新统计信息、回收死元组。这要求更为完整的事务可见性检查。

WAL 回放与崩溃恢复。postmaster 非正常退出后，startup process 需回放 WAL 日志将数据库恢复至一致状态。WAL 写入路径已疏通（#251 修复了 sync_file_range），但回放路径未经测试。

并行查询。PostgreSQL 自 9.6 版本引入的 parallel sequential scan 与 parallel hash join 需多个 backend 进程通过共享内存与信号协调。信号唤醒路径已修复（#247, #316），但完整的并行查询链路未经测试。

PostgreSQL 回归测试套件。`make check` 包含两百余个测试。跑通全套测试需前述未完成的各项工作基本完成。

### 4.4 关键结论

PostgreSQL 是理想的内核兼容性试金石。Web 服务器主要压力在 epoll 与 sendfile，而数据库测试的是整个 POSIX 模型: fork/exec 的进程生命周期、setresuid/setresgid 的凭证切换、SysV shm 的共享内存、AF_UNIX 与 SCM_RIGHTS 的进程间通信、epoll 与 self-pipe 的事件循环、fsync 与 rename 的持久化写入。任一项未能正确实现，PostgreSQL 均会以明确的错误信息退出，为内核开发者提供了精确的定位线索。

16 个 PR 所修复的均为通用内核缺陷，非 PostgreSQL 特有问题。凭证子系统（#246）影响所有使用 setresuid 的应用；epoll LT 修复（#504）影响所有使用电平触发 epoll 的事件循环（libuv, Go runtime, nginx）；VFS rename 修复（#312）影响所有使用 durable_rename 模式进行原子写入的应用（SQLite, 各类包管理器）。PostgreSQL 是首个系统性暴露这些问题的应用。
