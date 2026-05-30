# BigLab B 总结报告

**作者**: Joseph Joshua (jsph273)
**日期**: 2026-05-30

## 1. 概述

本报告总结在 StarryOS（基于 ArceOS 模块构建的 Linux 兼容内核）上为期六周的内核开发工作。核心产出为: 构建了一套 AI 驱动的内核开发流水线（starry-harness），完成了 32 个系统调用修复与功能实现的 PR，并在此过程中使 PostgreSQL 和 Weston 两个大型 Linux 应用在 StarryOS 上达到基本可运行状态。

工作主线如下:

```
实验1 (AI 流水线) ──→ 实验2 (系统调用修复) ──→ 实验4 (大型应用适配)
       │                        │                        │
   starry-harness          32 个已合入 PR           PostgreSQL 启动
   9 技能 + 3 代理         覆盖 20+ syscall          Weston 14 渲染
   16 脚本                 7 个新功能实现
                          25 个语义修复
```

32 个 PR 已合入 [rcore-os/tgoskits](https://github.com/rcore-os/tgoskits) 的 `dev` 分支，时间跨度 2026-04-15 至 2026-05-21。涉及子系统: 信号处理、进程凭证、VFS、内存管理、epoll、netlink、DRM/KMS、evdev、timerfd、memfd、IPC、aarch64 架构支持。新增回归测试用例 30 余个，覆盖 riscv64、aarch64、x86_64、loongarch64 四个架构。

各实验详细报告:
- [实验1: AI 开发流水线](exp1_pipeline/report.md)
- [实验2: 系统调用修复与分析](exp2_syscall/report.md)
- [实验4: 大型应用适配](exp4_bigapps/report.md)（含 [PostgreSQL](exp4_bigapps/postgresql.md) 与 [Wayland/Weston](exp4_bigapps/wayland.md) 独立深度分析）

## 2. 实验1: AI 开发流水线

构建了 `starry-harness`，一个基于 Claude Code 插件架构的 AI 驱动内核开发流水线。其设计原则为: 将内核开发中高重复性、低决策需求的工作（测试生成、Linux 行为对照、代码风格审查、报告撰写）交由自动化流程处理，开发者保留架构决策与语义判断的控制权。

流水线由 9 个技能、3 个专业代理、16 个基础设施脚本构成。核心工作流: 发现目标（hunt-bugs）→ 生成测试 → Linux 参照对比（linux-comparator）→ 定位根因并修复 → 代码审查（kernel-reviewer）→ 生成报告（report）→ 准备提交（start-submission）。

三道工程护栏: 其一，所有代理输出须通过 JSON Schema 验证，确保下游可无歧义消费；其二，`known.json` 作为所有已发现缺陷的单一事实来源，维护状态机以防止重复修复；其三，代码审查代理独立运行，其输出为"发现问题"而非"批准通过"，不合格的修复退回修改直至全部审查维度通过。

详见 [实验1 完整报告](exp1_pipeline/report.md)。

## 3. 实验2: 系统调用修复

完成 24 个 PR，覆盖信号处理、进程凭证、VFS、内存管理、epoll、IPC、网络、定时器等核心子系统。这些修复的共同模式为: StarryOS 在基本调用路径上的行为正确，但在边界条件处理、错误路径语义、与 Linux 的精确 errno 对齐上存在系统性偏差。

以下为若干代表性案例。

**SA_RESTART 系统调用重启语义（#247）。** 信号处理函数设置 `SA_RESTART` 标志后，被中断的阻塞系统调用应自动重启。实现此语义需同时处理三个条件: 将 PC 回退至 syscall 指令（RISC-V/AArch64 上 `ecall`/`svc` 为 4 字节，x86_64 上 `syscall` 为 2 字节）；恢复 `a0` 寄存器为原始首参数值（因 RISC-V/AArch64/LoongArch64 上 `a0`/`x0` 同时担任首参数与返回值寄存器，中断时已被 `-EINTR` 覆盖）；不可调用 `set_retval(0)` 清除错误码（该操作在 RISC-V/AArch64 上会覆盖刚恢复的 `a0`）。x86_64 因 `rdi` 与 `rax` 为不同寄存器而不受此约束，但实现须对所有架构保持一致性。

**进程凭证子系统（#246）。** 实现了完整的 `struct cred` 模型: uid、gid、euid、egid、suid、sgid、fsuid、fsgid 八个身份字段及补充组列表。权限检查接入 `faccessat2`（基于 fsuid 而非 euid）、`fchownat`、`fchmodat`、`kill`/`tkill`/`tgkill`、IPC 子系统、`/proc/<pid>/status`。`setresuid` 的 NOCHG 语义（传入 -1 表示该字段不修改）、`setreuid` 中 suid 的自动更新规则，均为容易被忽略但应用广泛依赖的细节。

**epoll 电平触发模式就绪队列（#504）。** LT 模式下 `epoll_wait(maxevents=N)` 对单个就绪 fd 返回 N 份副本。根因为 `pop_front` + `push_back` 循环导致同一 `EpollInterest` 被反复消费。修复对齐 Linux `ep_send_events` 模型: `mem::take` 一次性排空 `ready_queue` 至本地 `txlist`，每个 interest 仅访问一次，LT 留存的放入 `keep` 队列，循环结束后 splice 返回。同步修复了 `consume()` 与 `register()` 之间的事件丢失窗口。

**VFS rename 操作中 DirEntry 的保持（#312）。** tmpfs 上 `rename` 后目标文件内容变为全零。根因为 `DirNode::rename` 对源路径调用了 `forget_entry`，将 DirEntry 从缓存移除后 page cache 被释放。修复将源 DirEntry 自源目录取出（非 forget）并以目标文件名插入目标目录，保持 DirEntry 实例不变。

**IPC 共享内存死锁（#226）。** SMP=4 条件下 95% 复现率。`sys_shmget` 与 `sys_shmat` 加锁顺序相反（SHM_MANAGER → shm_inner vs. shm_inner → aspace → SHM_MANAGER），构成 AB/BA 死锁。修复统一全局锁顺序为 SHM_MANAGER → shm_inner → aspace，`sys_shmdt` 采用分阶段加锁重构。同步修复了 `clear_proc_shm` 缺失 `aspace.unmap()` 导致的 use-after-free。

详见 [实验2 完整报告](exp2_syscall/report.md)。

## 4. 实验4: 大型应用适配

使 PostgreSQL 与 Weston 在 StarryOS 上达到基本可运行状态。PostgreSQL 测试的是 POSIX 模型（进程、凭证、IPC、文件系统语义），Weston 测试的是 Linux 驱动模型和用户态图形生态（DRM/KMS、evdev、netlink、sysfs）。两者覆盖了 StarryOS 内核接口的互补侧面。

### 4.1 PostgreSQL

PostgreSQL 的启动过程相当于一次 POSIX 兼容性的系统性扫描。适配分三阶段推进:

| 阶段 | 关键阻塞 PR | 暴露的内核缺陷 |
|------|------------|---------------|
| postmaster 启动 | #313, #311, #246, #249 | AF_UNIX socket 属主错误、SOCK_STREAM EOF 语义错误、凭证子系统缺失、PR_SET_PDEATHSIG 未实现 |
| initdb | #251, #312, #319, #248 | fsync 对目录 fd 返回 EISDIR、rename 后内容丢失、prlimit64 静默无效、RLIMIT_STACK 默认值过小 |
| 运行查询 | #314, #504, #226, #316 | EPOLL_CTL_MOD 后事件丢失、epoll LT 重复事件、SMP 下 IPC 死锁、interrupt waker 竞态 |

PostgreSQL 适配过程中暴露的 StarryOS 问题可归为三个层次。其一为缺失的功能（凭证子系统 #246，整套安全模型须从头构建）。其二为语义偏差（绝大多数 PR）: 系统调用存在且基本路径可通，但某个标志位组合、某个 errno 条件、某个边界情况与 Linux 不一致。其三为并发正确性问题（#226, #316, #504）: 单核行为正确，SMP 条件下暴露竞态或死锁。

### 4.2 Wayland / Weston

Weston 的适配按子系统分层推进。设备节点层（#506, #508, #513）: 创建 `/dev/dri/card0`（DRM/KMS 双路径）和 `/dev/input/eventN`（evdev 多设备暴露），实现 sysfs class 软链结构和 `/run/udev/data/` 预填空。输入子系统层（#513, #509）: 实现 EVIOCGPROP/EVIOCGABS ioctl，接入 virtio-input IRQ 唤醒替代 16ms tick 兜底，修复 EventDev::register 的无条件 wake 死循环。图形协议层（#506, #507）: 实现 memfd F_SEAL_* 全套 seal 语义（渗透到 ftruncate/write/mmap 三个独立入口），实现 AF_UNIX SOCK_STREAM 的 SCM_RIGHTS 和 MSG_DONTWAIT。网络层（#512）: 放开 AF_NETLINK 的协议号与套接字类型限制，合成 rtnetlink 应答。架构层（#510, #511）: 使能 aarch64 EL0 cache 指令，增加 GICv3+CNTV 支持 Apple HVF。

Weston 适配的核心困难不在于内核修改本身，而在于定位问题需要穿透五层抽象（Weston → libinput/libdrm → libudev → sysfs/evdev → 内核）。三个典型案例: sysfs class 目录须为软链（libudev 依赖 `realpath()` + `dirname()` 定位 device 容器）；evdev minor 号须从 64 起（`EVDEV_MINOR_BASE`，否则 libinput 的 `fstat().st_rdev` 反查失败）；`/run/udev/data/` 预填空文件须存在（libudev 的 `get_is_initialized()` 仅检查文件存在性）。这三项均非系统调用层面的问题。

### 4.3 对比

| 维度 | PostgreSQL | Weston |
|------|-----------|--------|
| 核心依赖 | 进程模型、凭证、VFS、IPC、epoll | 驱动节点、DRM/KMS、evdev、netlink、AF_UNIX |
| 失败模式 | 明确错误码与日志输出 | 多层级初始化失败，根因深埋 |
| 修复策略 | 按 syscall 逐个修复，PR 独立可合入 | 按子系统分层推进，PR 间存在依赖关系 |
| PR 数量 | 16 个（含与实验2 共享的 syscall 修复） | 8 个（Wayland 路径专属） |

详见 [实验4 合并报告](exp4_bigapps/report.md)，以及 [PostgreSQL 深度分析](exp4_bigapps/postgresql.md) 与 [Wayland/Weston 深度分析](exp4_bigapps/wayland.md)。

## 5. StarryOS 架构分析

32 个 PR 的完成过程同时是对 StarryOS 架构的深入理解过程。每个缺陷的根因最终追溯至某个设计决策，每个修复方案均受架构约束。以下基于这一过程中的观察，分析 StarryOS 架构中有利于和不利于 AI 辅助开发的特征。

### 5.1 架构概览

StarryOS 并非从零构建的独立内核。它在 ArceOS unikernel 的模块体系（axhal、axtask、axmm、axfs、axnet、axdriver）之上叠加了一层 Linux syscall ABI 兼容层。五层结构: 平台抽象层（ax-plat，8 个板级实现）→ ArceOS 模块层 → Starry 组件层（starry-process/signal/vm）→ 内核库层（starry-kernel，约 210 个 syscall）→ 用户空间层其实质是在复用 ArceOS 的硬件抽象、驱动、调度器、页表、文件系统的前提下，通过约 640 行的巨型 match 分发器和围绕它的辅助模块实现完整的 Linux syscall 语义。

### 5.2 有利于 AI 开发的架构特征

分层模块化使每层可独立验证。修改 `starry-signal` 无须理解 ArceOS 的 virtio 驱动。`starry-process` 作为纯数据模型，不依赖调度器与 MMU，其单元测试结果在 no_std 内核环境中同样成立。此特征对于 AI 辅助开发尤为重要: AI 代理可仅阅读相关层的源码而不必加载整个内核上下文，显著提高了分析的精确度。

Rust 类型系统的编译时检查在此架构中被系统性利用。内存管理子系统的四种映射后端（Linear/Cow/Shared/File）通过 `enum_dispatch` 统一为 `Backend` 枚举，编译器强制穷举所有变体，新增变体时自动标记所有未处理分支。此特征为 AI 修改内核代码提供了安全网: 许多逻辑错误在编译期即被捕获。

信号投递采用同步内联检查模型。每次 syscall/异常/缺页返回后在执行循环中调用 `check_signals()`，而非异步抢占模型。结果: 信号投递时机完全由 syscall 状态驱动，可复现、可测试，不存在竞态窗口。SA_RESTART 的 PC 回退（x86_64 上 2 字节，其余架构 4 字节）是纯算术操作。同步信号模型使得 AI 对信号相关修复的分析与测试变得直接，无须处理异步抢占引入的时序不确定性。

### 5.3 产生系统性摩擦的架构特征

系统调用分发器是一个无模块边界的巨型 match。修复某个 syscall 的边界检查需要在此 match 附近操作，但无法单独测试该 syscall。验证路径为"构建内核 → 制作 rootfs → 启动 QEMU → 运行测试"，单次迭代需数分钟。若每个 syscall 是独立的可注册模块，单个 syscall 的单元测试和 fuzz 将不需要完整的内核启动流程。

层间接口依赖隐式副作用。`TaskExt::on_enter/on_leave` 通过 `scope_local` 的隐式状态切换传递进程资源上下文，无显式返回值。调度器与进程管理器之间的契约由 Cargo 特性组合与运行时副作用定义，非类型系统可验证。32 个 PR 中涉及调度与进程交互的修复（凭证同步、信号投递时机、fd 作用域泄漏），大部分根因可追溯至这种隐式耦合。

缺乏能力发现机制。`userfaultfd`、`io_uring_setup`、`memfd_secret` 等 8 个 syscall 返回空操作的匿名 fd（调用成功但无实际功能）。`fanotify_init` 返回 `-ENOSYS` 反而提供了明确的不可用信号。Dummy fd 策略使用户程序误认为所请求功能可用，这是最危险的失败模式: 调用方无法通过返回值判断操作是否实际生效。

`#[extern_trait]` 虽解决了 no_std 下 trait object ABI 不稳定问题，但跨 crate 的调用契约由 proc-macro 生成的 C 兼容虚表定义，编译器无法验证实际接口一致性。修改 trait 定义后遗漏更新实现了该 trait 的 crate，将在运行时表现为未定义行为而非编译错误。

### 5.4 改进方向

若重新设计，核心思路是使架构内建更多"变更可被验证"的机制。

可插拔的 syscall 注册表。每个 syscall 声明其编号、参数签名、返回值类型，注册至可被 `/proc` 查询的表中。新增 syscall 无须触碰分发器，syscall 的单元测试和 fuzz 可在独立环境中运行。

显式的能力矩阵。`/proc/sys/kernel/capabilities` 使测试框架查询功能可用性，据此动态决定执行哪些测试用例。此机制对 AI 开发流水线尤为关键: AI 代理可在修改前查询能力边界，避免在不可用的功能上浪费分析资源。

层级间显式接口契约。`TaskExt` 的方法应有返回值传递状态变更，关键 invariants 通过 `debug_assert!` 在测试构建中验证。编译器可检查的接口契约比隐式副作用更适合 AI 参与的开发流程。

将 Linux 差异测试从外部工具升级为架构内建机制。每个 PR 自动在 Docker Linux 上运行相同测试输入，输出差异自动分类为已知偏差、待定问题或功能回归。此机制将显著缩短 AI 流水线中"发现 → 验证"的反馈循环。

## 6. 四类典型缺陷

回顾全部 32 个 PR，StarryOS 的系统调用实现存在四类反复出现的缺陷模式。

**基本路径正确，边界条件静默失效。** `prlimit64` 提高硬限制时返回成功但不修改限制值（#319）；`clock_gettime` 对非法 clock_id 静默返回当前时间（#208）；`getgroups(size=0)` 被当作错误拒绝（#208）。此类缺陷的危害最大: 调用方无法通过返回值判断操作是否实际生效。

**遗留实现与新语义的混淆。** `preadv`/`pwritev` 的 64 位偏移拆分为 `pos_l` 和 `pos_h` 两个参数，实现仅读取了 `pos_l`（#197）。从早期简化实现演进至完整 Linux ABI 的过程中，参数数量的变化容易被遗漏。`sys_pwritev2` 自 `sys_preadv2` 复制后内部调用仍为 `.read_at()`（#197），复制粘贴产生的实现未经对照核验。

**直觉驱动的边界处理。** `sys_sigaltstack` 使用 `<= MINSIGSTKSZ` 拒绝恰好等于最小值的大小（#207）；epoll_pwait 的 sigsetsize 严格等于检查拒绝了 musl libc 的 16 字节输入（#250）。Linux 的这些边界行为经过了多年的缺陷修复与回归测试，其精确语义非直觉可替代。

**错误路径上的副作用。** `prlimit64` 在无法提高限制时返回 `Ok(0)` 而非错误码（#319）；`rename` 的 `forget_entry` 在错误路径上释放了不应释放的 page cache（#312）。最危险的错误处理模式为"调用看似成功但内核状态未发生任何改变"。

## 7. 最终产出

```
实验1 (AI 流水线):
  ├── starry-harness 插件 (9 技能 + 3 代理 + 16 脚本)
  └── 支撑 32 个 PR 的开发、测试、审查全流程

实验2 (系统调用修复):
  ├── 24 个已合入 PR
  ├── 覆盖子系统: signal, cred, vfs, epoll, timer, ipc, mem, net, prctl
  ├── 7 个新功能: mremap, timerfd, SA_RESTART, PDEATHSIG, credentials,
  │     sync_file_range, user crash dump
  └── 20+ 回归测试用例

实验4 (大型应用):
  ├── 8 个 Wayland/Weston 专属 PR (#506 至 #513)
  ├── 复用实验2 中 10 余个 PR 完成 PostgreSQL 适配
  ├── PostgreSQL: postmaster 启动, initdb, 基本查询通路
  └── Weston 14: drm-backend + libinput + wayland 协议渲染通路
```

## 8. 个人反思

六周的工作改变了我对"操作系统兼容性"的理解。此前认为兼容性即实现 syscall 表: 对照 Linux 的 syscall 编号列表逐一实现，跑通即完成。实际经验表明并非如此。

兼容性的真正难点不在系统调用的存在性，而在每个系统调用的精确语义、每个错误条件的 errno 返回值、每个边界情况的处理方式，以及用户态生态中那些从未被集中文档化的隐性契约。`/sys/class/drm/card0` 须为指向 `/sys/devices/virtual/drm/card0` 的软链；`/run/udev/data/` 中须预填空白初始化文件；evdev 的 minor 号须为 `64+i` 而非 `i+1`。这三个问题的共同特征是: 它们都不对应任何一个缺失的系统调用。它们均失败于实现了一个功能但与 Linux 的隐性约定存在毫厘之差。

关于 AI 辅助开发的定位，32 个 PR 的完成过程中产生了较为具体的认识。AI 承担了约 70% 的执行工作量（编写代码、生成测试、修改配置、运行 clippy），但约 90% 的决策工作量（判断根因、确定修复方向、选择锁策略、评估传递性影响）依赖人工判断。AI 的核心价值在于消除"已知问题所在但修改成本过高"的摩擦，而非替代"问题尚不明确需要探索"的认知过程。将此比例关系厘清，对于在 AI 能力持续发展的背景下设计操作系统课程具有参考意义。
