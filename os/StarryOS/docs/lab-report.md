# StarryOS

## 一、多核支持能力分析与优化方案

### 1.1 当前多核支持现状

StarryOS 自身不含 SMP 启动相关的汇编代码，多核启动、调度器、硬件抽象层均由底层 ArceOS 框架（ax-runtime、ax-task、ax-hal）实现。StarryOS 通过 Cargo feature `smp` 启用多核模式，内核层的职责是确保自身数据结构的多核安全性。

调度策略采用 ArceOS 提供的 Round-Robin，每个 CPU 核心维护独立的运行队列。新任务创建时通过原子计数器按 round-robin 方式分配到各核，此后便固定在该核上运行。ax-task 源码中有 TODO 注释明确标注 load balancing 尚未实现，空闲核进入 idle 循环，不会从其他核的队列窃取任务。

CPU 亲和性通过 sched_setaffinity/sched_getaffinity 支持，底层以 AxCpuMask 位掩码实现。但 syscall 层存在限制，仅允许设置当前线程的亲和性，对其他线程的请求返回 EPERM（schedule.rs:96 有对应 TODO）。

同步机制方面，StarryOS 混合使用多种原语。SpinNoIrq 为关中断自旋锁，用于中断上下文可能访问的数据结构，如 futex 等待队列。Mutex 为阻塞互斥锁，保护地址空间等需要长时间持有的资源。RwLock 保护 FD 表，允许并发读取。此外广泛使用 core::sync::atomic 原子操作。Futex 实现较为完整，支持 private/shared、bitset、requeue 操作。

并发热点方面，有以下几处值得关注。全局共享 futex 表（futex.rs:269）为单一 `Mutex<FutexTables>`，所有跨进程 futex 操作均在该锁上序列化。地址空间以 `Arc<Mutex<AddrSpace>>`（task/mod.rs:197）保护，缺页处理、mmap、munmap 均需获取该锁，同进程多线程之间会产生竞争。IPC 子系统的消息队列和共享内存也各用一个全局 BTreeMap 加 Mutex 管理。

### 1.2 优化方案

调度器增强是最直接的优化方向。可以在 ax-task 层面引入工作窃取机制，使空闲核从繁忙核的运行队列中获取任务。周期性负载均衡也有必要，每隔若干 tick 检查核间队列长度差异并执行迁移即可，不需要复杂的算法。更进一步可以实现 CFS 或 O(1) 调度器以支持更灵活的调度策略。

锁粒度可以进一步细化。地址空间锁可拆分为 per-VMA 锁，使不同虚拟地址区域上的操作互不阻塞，Linux 6.4 已采用此方案。共享 futex 表可改为分段哈希表，按地址哈希分到不同 bucket，每个 bucket 独立加锁。FD 表在高并发 open/close 场景下也可改为 per-slot 原子操作。

TLB 管理方面，当前未做跨核 TLB 一致性管理。可实现 targeted TLB shootdown，仅向运行受影响地址空间的核心发送失效通知。munmap 和 mprotect 的 TLB 失效请求可进行批量合并。

对于进程表查找、FD 表读取等读多写少的热路径，可以引入 RCU 机制，使读操作完全无锁。

### 1.3 最值得改进的点

综合源码 TODO/FIXME 标记和自动化测试结果，以下按重要性排列了十个改进方向。

多线程 execve 处理是最关键的问题。execve.rs:50-53 中，当 `proc_data.proc.threads().len() > 1` 时直接返回 WouldBlock，但 Linux 规范要求 execve 先终止同进程内所有其他线程再替换进程映像。任何在多线程存活期间调用 execve 的程序都会失败。

作业控制同样紧迫。signal.rs:31 中 SIGSTOP 的处理分支直接调用 `do_exit(1, true)` 终止进程。按 Linux 语义，SIGSTOP 应将进程置为暂停状态，待 SIGCONT 恢复。当前实现导致 Ctrl+Z 直接杀死前台进程。修复需要为任务引入 STOPPED 状态，并在 wait4 中正确处理 WUNTRACED 和 WCONTINUED 标志。

pwritev/pwritev2 存在 copy-paste 错误。io.rs:210-222 中 sys_pwritev2 的函数体与 sys_preadv2 完全相同，均调用 `f.inner().read_at(...)`，写操作实际执行的是读操作。自动化测试已验证此缺陷，写入数据后回读内容未发生改变。将 read_at 改为 write_at 即可修复，仅涉及一行代码。

mremap 因两个 bug 完全不可用。mmap.rs:306 在调用 sys_mmap 时传入 `MmapFlags::PRIVATE.bits()` 但未包含 `MAP_ANONYMOUS`，同时 fd 设为 -1。sys_mmap 在第 123 行执行 `map_flags.contains(ANONYMOUS) != (fd <= 0)` 检查时，`false != true` 成立，直接返回 EINVAL。9 个测试用例中所有 mremap 调用均失败。此外 mmap.rs:300 存在变量遮蔽问题，函数参数 `flags: u32`（用户传入的 MREMAP_MAYMOVE 等标志）被 `let flags = aspace.find_area(addr)?.flags()` 覆盖，导致用户标志完全丢失。

fcntl F_GETFL 的访问模式推导逻辑存在错误。fd_ops.rs:264-271 通过读取文件 stat.mode 权限位来判断 O_RDONLY/O_WRONLY/O_RDWR，而非保存并返回 open 时传入的标志。测试中以 O_RDONLY 打开一个权限为 0644 的文件，F_GETFL 返回 accmode=2（O_RDWR），因为文件 owner 权限同时包含读和写。正确做法是在 File 结构体中保存 open 时的 flags。

copy_file_range 在同文件前向重叠场景下会损坏数据。do_send 函数（io.rs:264）使用 4096 字节缓冲区执行读写循环。当源和目标位于同一文件且范围前向重叠时（目标偏移大于源偏移），首次写入会覆盖尚未读取的源数据，导致后续读取到已被修改的内容。测试在 8192 字节文件上将 offset 0 起 6000 字节复制到 offset 2000，结果 file[6096] 处的值为 0x30（来自原始偏移 2096），而正确值应为 0x00（来自原始偏移 4096）。

flock 未实现。fd_ops.rs:308-311 仅打印 debug 日志后返回 `Ok(0)`，不维护锁状态，不校验参数。fcntl 的 F_SETLK/F_SETLKW 同为空操作。测试验证了排他锁无法阻止其他排他锁或共享锁的获取。

memfd_create 以 /tmp 目录下的真实文件模拟匿名内存对象。memfd.rs:17-28 遍历 0x0000 到 0xFFFF 寻找可用的 `/tmp/memfd-XXXX` 文件名并创建。fstat 显示 st_nlink 为 1，关闭 fd 后文件仍保留在 /tmp 中。基本的读写、mmap、cloexec 功能可以正常使用。代码开头标注了 `// TODO: correct memfd implementation`。

权限系统为空实现。sys.rs 中 sys_getuid 和 sys_geteuid 直接返回 0，所有进程始终以 root 身份运行。ctl.rs:33-36 的 sys_capget 将 effective、permitted、inheritable 三个字段均填充为 u32::MAX。

POSIX Timer 未实现。syscall/mod.rs:630 对 timer_create、timer_gettime、timer_settime 统一返回 Ok(0)。timerfd_create 返回无功能的 dummy fd。

## 二、Linux Syscall 支持能力与缺陷分析

### 2.1 总体情况

syscall/mod.rs 的中央分发表包含 210 个 Sysno 变体，对应 209 个独立系统调用（newfstatat 与 fstatat 为同一调用在不同架构上的名称）。其中 22 个通过 `#[cfg(target_arch = "x86_64")]` 条件编译限定，为 x86_64 特有的 legacy 接口（open、stat、mkdir、fork、pipe、select 等），1 个仅 riscv64 可用（riscv_flush_icache）。

文件系统类 syscall 覆盖率最高，约 83 个变体，完成度约 95%。open/close/read/write/stat/lseek/dup 等基本操作均已实现，readv/writev/pread64/pwrite64/sendfile/splice 等高级 IO 接口亦完备。IO 多路复用（select、poll、epoll 全套）基本完整。

内存管理包含 10 个核心 syscall，大部分已实现。mmap 的实现质量较高，自动化测试中 8 个用例通过 7 个。COW 写时复制隔离性正确（MAP_PRIVATE 映射写入不影响原文件），MAP_FIXED_NOREPLACE 能正确返回 EEXIST，MAP_SHARED 匿名映射、文件映射、输入参数校验均工作正常。唯一失败的是 MAP_ANONYMOUS 搭配正 fd 值时被拒绝。Linux 在设置 MAP_ANONYMOUS 时忽略 fd 参数，而 StarryOS 的校验更为严格。mremap 因前述两个 bug 完全不可用。madvise 和 msync 为空操作，直接返回 0。

进程管理完成度约 80%。clone/clone3 已实现，但命名空间相关标志（CLONE_NEWPID 等）仅为 stub。execve 不支持多线程进程。信号处理完成度约 90%，rt_sigaction、rt_sigprocmask、rt_sigreturn、kill、tkill、tgkill 均在，但 core dump 与 stop/continue 未实现。

网络包含 16 个 syscall，完成度约 85%。socket、bind、connect、listen、accept4、sendto、recvfrom 等核心调用均已实现，支持 AF_UNIX 和 AF_INET（IPv4），不支持 IPv6。

IPC 方面，System V 消息队列和共享内存有基本实现，但阻塞式 msgsnd/msgrcv 未完成，队列满时直接返回 EAGAIN，即使调用方未传 IPC_NOWAIT 标志。信号量（semget/semop/semctl）完全缺失。

另有 11 个 syscall 返回无功能的 dummy fd（包括 timerfd_create、inotify_init1、userfaultfd、io_uring_setup 等），用于使 QEMU 等复杂应用不至于直接崩溃。3 个 timer 相关调用直接返回 0。

### 2.2 通过自动化测试确认的缺陷

以下结果均经过实际测试流程验证，包括编写 C 测试用例、riscv64-linux-musl-gcc 交叉编译、通过 Docker 注入 ext4 rootfs 镜像、QEMU 启动执行。

pwritev2 的 copy-paste 错误（io.rs:210-222）在 1.3 节已做分析。sys_pwritev2 与 sys_preadv2 函数体完全一致，均调用 `read_at`。测试先用 pwrite 写入已知数据，再用 pwritev2 尝试覆写，回读后数据未发生改变。3 个用例中 1 个通过（pwrite 本身正确），2 个失败。

mremap 测试（mmap.rs:282-318）执行 9 个用例，7 个失败，均因 EINVAL。通过的 2 个为负面测试，验证了非页对齐地址和 new_size=0 被正确拒绝。

fcntl F_GETFL 测试（fd_ops.rs:256-273）执行 9 个用例，5 个失败。以 O_RDONLY 和 O_WRONLY 打开的 0644 文件均报告 accmode=2（O_RDWR）。管道的两个测试通过，原因是管道的 stat.mode 恰好与实际访问模式吻合。F_SETFL 设置 O_APPEND 无效的测试也失败了，写入数据覆盖了文件开头而非追加到末尾。另有一个测试中 `open(path, O_WRONLY|O_CREAT|O_TRUNC|O_APPEND, 0644)` 调用本身返回 -1，原因不明，可能与 FS 层对该标志组合的处理有关，未做进一步排查。

copy_file_range 测试（io.rs:317-352）执行 7 个用例。其中 3 个失败为内核缺陷（flags 非零被接受、同文件重叠数据损坏、管道作为输入被接受），1 个失败为测试代码自身的 bug（期望字符串有误），3 个通过（跨文件基本复制、零长度复制、超出 EOF 的复制）。数据损坏的具体分析见 1.3 节。另外两项失败对应的是代码中三条 TODO 注释标注的遗漏，即 flags 参数必须为 0（io.rs:323 的 `_flags` 参数未使用）且输入输出均需为普通文件。

memfd_create 测试（memfd.rs:13-32）执行 7 个用例，5 个通过。两个失败分别验证了 st_nlink 非 0（真正的 memfd 无目录条目）和关闭后 /tmp/memfd-\* 文件仍然存在。源码标注了 TODO 表明开发者已知此实现不正确。

flock 测试（fd_ops.rs:308-312）执行 8 个用例，4 个失败。排他锁无法阻止其他排他锁或共享锁的获取，无效参数 flock(fd, 0) 亦被接受。4 个通过的用例（单独加解锁、两个共享锁并存、解锁后重新加锁）均因 `Ok(0)` 恰好符合预期返回值，并不表示功能正确。

mmap 测试（mmap.rs:90-132）执行 8 个用例，仅 1 个失败。测试覆盖了匿名私有映射、文件映射读取、MAP_PRIVATE COW 隔离、MAP_FIXED_NOREPLACE、MAP_SHARED 匿名映射、length=0 拒绝、offset 非页对齐拒绝，全部正确。唯一失败的是 MAP_ANONYMOUS 搭配 fd=3（一个有效的已打开文件描述符）被拒绝。mmap.rs:123 的检查 `map_flags.contains(ANONYMOUS) != (fd <= 0)` 在 ANONYMOUS=true 且 fd=3 时计算得 `true != false`，返回 EINVAL。Linux 内核在 ANONYMOUS 模式下忽略 fd 参数。此为兼容性差异，实际影响有限，因为规范程序在使用 MAP_ANONYMOUS 时会将 fd 设为 -1。

### 2.3 Syscall 优先排序

排序依据三条原则。其一，修复现有 bug 优先于新增功能，因为修复改动量小（pwritev2 仅需修改一行，mremap 修改两三行即可恢复基本功能）而影响面大。其二，被更多程序隐式依赖的 syscall 排在前面。其三，能够使测试基础设施正常工作的功能需优先考虑，例如作业控制修复后才能在 shell 环境中进行更复杂的操作。

最高优先级五项，按顺序为修复 execve 多线程支持、修复 pwritev2 的 read_at/write_at 混淆、修复 mremap 的 MAP_ANONYMOUS 缺失及变量遮蔽、实现 SIGSTOP/SIGCONT、实现 flock 及 fcntl 文件锁。

高优先级五项，按顺序为修复 fcntl F_GETFL 访问模式推导逻辑、实现 waitid、实现 timer_create 系列、实现 semget/semop/semctl、实现 inotify 系列。

中优先级包括 timerfd、uid/gid 实际支持、execveat、mremap 完整实现（MREMAP_MAYMOVE、原地缩减）、/proc 完善。

低优先级包括 io_uring、IPv6、seccomp、AF_NETLINK、namespace 支持。

## 三、AI 自动迭代测试方法设计

### 3.1 整体思路

将 syscall 测试组织为可重复执行的迭代循环，每轮包含六个阶段：目标选择、测试生成、交叉编译、rootfs 注入、QEMU 执行、结果分析。各阶段均可由 AI 自动完成，人工仅在 bug 修复环节介入审查。

该方法在本次实验中实际执行了七轮迭代，覆盖 7 个 syscall 共 51 个测试用例，发现 5 个此前未知的 bug。

### 3.2 工具链实现

tools/ 目录下实现了三个 shell 脚本组成的测试管线。

compile.sh 执行交叉编译，优先检测本机 riscv64-linux-musl-gcc，未找到则回退至 Docker 容器。编译参数为 `-static -O2 -Wall`，静态链接确保测试程序在 StarryOS 上无动态库依赖。

inject.sh 将编译产物注入 rootfs 磁盘镜像。由于 macOS 无原生 ext4 挂载支持，该步骤通过 Docker 容器完成 mount、copy、umount 操作。注入时将测试二进制放置于 /starry_test 并生成 /test_runner.sh 脚本。StarryOS 的 init.sh 经过修改，启动时检测到 /test_runner.sh 即自动执行测试程序。

run.sh 从 workspace 根目录构建内核（`cargo starry build --arch riscv64`），将 ELF 转换为 raw binary 后启动 QEMU。使用 `-nographic` 串口模式，60 秒超时，输出保存至 tests/results/。

测试用例全部使用 C 语言编写。选择 C 的原因在于可直接调用 libc 的 syscall wrapper，更贴近真实应用的调用方式。静态链接后二进制约 128KB。测试框架为自定义的 starry_test.h 头文件，提供 TEST/TEND 宏（规避 C 预处理器的逗号解析问题）和 EXPECT_EQ、EXPECT_TRUE、EXPECT_ERRNO 等断言宏。每个测试输出 `PASS: suite::name` 或 `FAIL: suite::name (details)` 格式的结果行，便于脚本解析。

### 3.3 目标选择策略

自动选择测试目标时遵循三级优先排列。源码中标注 TODO/FIXME 的 syscall 优先级最高，因为开发者已标注的薄弱点出现 bug 的概率最大。其次是尚未被任何测试覆盖的 syscall，最后考虑实现复杂度较高的。

具体做法是交叉三个信息源。读取 tests/known.json 获取已测试 syscall 及结果，读取 kernel/src/syscall/mod.rs 获取已实现 syscall 清单，grep TODO/FIXME 定位标注的弱点。综合三者为候选目标评分排序。

### 3.4 测试用例设计原则

每个 syscall 的测试至少覆盖三类场景。正常路径验证合法参数下的返回值和副作用。错误路径验证非法参数是否返回正确的 errno。边界条件覆盖零长度、NULL 指针、未对齐地址等极端输入。涉及多个文件描述符交互的 syscall 还需增加冲突场景测试。

一条需要特别注意的原则是每个 TEST 块必须做好失败后的资源清理，防止 segfault 导致后续测试无法执行。mremap 的首版测试即遇到此问题，第一个用例中 mremap 返回 MAP_FAILED 后直接对该指针解引用，导致段错误，后续 8 个用例全部未能运行。增加 `if (p2 == MAP_FAILED) goto done;` 的防护后恢复正常。

### 3.5 知识库

tests/known.json 作为迭代状态的持久化存储，在每次测试完成后更新。记录内容包括测试状态（buggy、broken、mostly_ok、stub）、发现的 bug 清单、通过和失败的用例名称。下一轮目标选择时读取此文件，即可获知已覆盖范围。

### 3.6 Claude Code Skill 集成

整个测试流程被封装为 Claude Code 的 skill。执行 `/test-starry discover` 自动完成目标选择并展示推荐列表，执行 `/test-starry mmap` 则针对指定 syscall 走完从测试生成到结果分析的完整流程。本实验的七轮测试均通过该 skill 驱动执行。

Skill 定义位于 .claude/skills/test-starry/ 目录，描述了测试流程各阶段的操作规范、harness 使用约定、编译运行命令以及结果分析格式。

### 3.7 局限性

当前所有测试均在单进程内运行。涉及 fork 的场景难以测试，子进程与父进程的输出交错混合后不易解析。并发相关的 bug 通过单线程测试基本无法触发。QEMU 与真实硬件在时序上存在差异，定时器精度等问题无法在 QEMU 环境中复现。

此外，测试覆盖范围限于 syscall 的功能正确性。性能问题、资源泄漏、极端并发下的竞态条件等均超出当前测试方法的能力范围。

## 四、实验结果汇总

七轮自动化测试的数据汇总如下。

| syscall         | 用例数 | 通过 | 失败 | 发现情况                   |
| --------------- | ------ | ---- | ---- | -------------------------- |
| pwritev2        | 3      | 1    | 2    | read_at/write_at 混淆      |
| mremap          | 9      | 2    | 7    | 全部调用返回 EINVAL        |
| fcntl F_GETFL   | 9      | 4    | 5    | 访问模式从权限位推导       |
| copy_file_range | 7      | 3    | 4    | 同文件重叠导致数据损坏     |
| mmap            | 8      | 7    | 1    | 仅 fd 校验偏严             |
| memfd_create    | 7      | 5    | 2    | /tmp 文件模拟（已知 TODO） |
| flock           | 8      | 4    | 4    | 完全未实现（已知 TODO）    |

51 个测试用例中 26 个通过、25 个失败。

需要对发现的性质做区分。确认为此前未知的代码错误共 5 个，分别是 pwritev2 调用 read_at、mremap 缺少 MAP_ANONYMOUS、mremap 变量遮蔽、F_GETFL 从文件权限位推导访问模式、copy_file_range 同文件前向重叠数据损坏。这些问题在源码的 TODO/FIXME 注释中均未提及。

其余测试失败分为两类。一类是确认了代码中已有 TODO 标注的已知限制，包括 flock 空实现、memfd_create 的 /tmp 模拟方式、copy_file_range 缺少 flags 校验和文件类型检查。另一类是 Linux 兼容性层面的细微差异，如 mmap 对 MAP_ANONYMOUS 搭配正 fd 值的处理比 Linux 更为严格。另有 1 个失败为测试代码自身的期望字符串错误。

mmap 的测试结果值得特别关注。8 个用例中 7 个通过，表明 StarryOS 内存管理核心路径的实现质量较高，COW 隔离、FIXED_NOREPLACE、共享匿名映射、文件映射等关键功能均正确。问题集中在边缘场景和辅助 syscall（mremap）上。
