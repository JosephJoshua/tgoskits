# 实验4: 大型应用适配

## 4.1 概述

使两个大型 Linux 应用在 StarryOS 上运行: PostgreSQL（关系型数据库）和 Weston（Wayland 合成器）。前者测试 POSIX 模型: 进程生命周期、凭证系统、IPC、文件系统语义、事件循环。后者测试 Linux 驱动模型和用户态图形生态: DRM/KMS、evdev、netlink、sysfs、AF_UNIX SCM_RIGHTS。

24 个 PR 支撑了两个应用的适配路径（16 个与 PostgreSQL 交互中发现，8 个为 Wayland 专属）。以下为每个应用的适配概要。完整技术细节和逐 PR 分析见独立报告。

## 4.2 PostgreSQL

详见 [postgresql.md](postgresql.md)。

### 适配路径

PostgreSQL 的启动过程相当于一次 POSIX 兼容性的系统性扫描。每个后台工作进程 fork 之后执行 setresuid 切换身份、AF_UNIX socketpair 建立 IPC、SysV shmget 分配共享内存、epoll + self-pipe 构造事件循环、fsync + rename 实现持久化写入。这些子系统任一未能正确运作，启动进程即退出并输出精确的错误信息。

适配分三个阶段推进。postmaster 启动阶段消除了凭证系统缺失（setresuid 空操作）、AF_UNIX socket 属主错误（chmod 返回 EPERM）、SOCK_STREAM EOF 语义错误（EAGAIN 而非 0）、PR_SET_PDEATHSIG 未实现等问题。initdb 阶段修复了 fsync 对目录 fd 返回 EISDIR、rename 后内容丢失、prlimit64 静默失效、RLIMIT_STACK 默认值过小等问题。运行查询阶段处理了 epoll CTL_MOD 事件丢失、epoll LT 重复事件、SMP 下 IPC 死锁、interrupt waker 竞态等并发正确性问题。

### 核心 PR

| PR | 子系统 | 问题 |
|----|--------|------|
| #246 | 凭证 | 实现完整 `struct cred` 模型（8 个 ID 字段 + 补充组列表），接入全部权限检查点 |
| #247 | 信号 | 实现 SA_RESTART: RISC-V/AArch64 上 a0 寄存器复用需精确恢复 PC 和参数 |
| #226 | IPC | 解决 SHM_MANAGER/shm_inner AB/BA 死锁: 统一锁顺序，分阶段加锁重构 |
| #312 | VFS | tmpfs rename 后 page cache 丢失: 源 DirEntry 须取出而非 forget |
| #314 | epoll | EPOLL_CTL_MOD 后旧 Arc 的 Weak 被丢弃: 替换前将新指针插入就绪队列 |
| #504 | epoll | LT 模式返回 maxevents 份副本: drain txlist → 每 interest 访问一次 → splice keep |

### 关键洞察

16 个 PR 所修复的均为通用内核缺陷。凭证子系统影响所有使用 setresuid 的应用；epoll LT 修复影响 libuv、Go runtime、nginx；VFS rename 修复影响 SQLite 及各类使用 durable_rename 的包管理器。PostgreSQL 是首个系统性暴露这些问题的应用。

## 4.3 Wayland / Weston

详见 [wayland.md](wayland.md)。

### 适配路径

Weston 的启动路径穿透了 Linux 用户态图形栈的全部层次。从 libdrm 打开 `/dev/dri/card0` 枚举 KMS 资源，到 libudev 扫描 sysfs 发现输入设备，到 libinput 通过 evdev ioctl 查询设备能力，到 Wayland 协议通过 SCM_RIGHTS 传递文件描述符。每个环节都要求内核提供精确的接口行为。

适配按子系统分层推进。设备节点层: 创建 `/dev/dri/card0`（DRM/KMS）和 `/dev/input/eventN`（evdev 多设备暴露），实现 sysfs 软链结构和 `/run/udev/data/` 预填空。输入子系统层: 实现 EVIOCGPROP/EVIOCGABS，接入 virtio-input IRQ 唤醒替代 16ms tick 兜底，修复 EventDev::register 的无条件 wake 死循环。图形协议层: 实现 memfd F_SEAL_* 全套 seal 语义（渗透到 ftruncate/write/mmap 三个入口），实现 AF_UNIX SOCK_STREAM 的 SCM_RIGHTS 和 MSG_DONTWAIT。网络层: 放开 AF_NETLINK 的协议号和套接字类型限制，合成 rtnetlink 应答。架构层: 使能 aarch64 EL0 cache 指令（UCT/DZE/UCI），增加 GICv3 + CNTV 支持 Apple HVF。

### 核心 PR

| PR | 子系统 | 问题 |
|----|--------|------|
| #506 | DRM | 创建 `/dev/dri/card0`，实现 legacy 与 atomic KMS 双路径 |
| #508 | sysfs | sysfs class 软链、evdev minor=64、/run/udev/data/ 预填空 |
| #509 | 综合 | PROT_WRITE 补齐读权限、IRQ waker、SCM_RIGHTS byte-mark 语义、MSG_DONTWAIT |
| #507 | memfd | F_SEAL_SHRINK/GROW/WRITE/SEAL 全部实现，并发安全的 seal 发布 |
| #512 | netlink | NETLINK_ROUTE/GENERIC/KOBJECT_UEVENT 放开，合成 rtnetlink 应答 |
| #513 | evdev | 每设备暴露 eventN，EVIOCGPROP/EVIOCGABS 透传 prop 位和 abs 范围 |

### 关键洞察

Weston 适配的核心困难不在于内核修改本身（所有修复加起来数百行），而在于定位问题需要穿透五层抽象（Weston → libinput/libdrm → libudev → sysfs/evdev → 内核），每层都可能隐藏一个微小的格式偏差。三个典型案例:

sysfs class 目录必须是软链而非普通目录。libudev 通过 `realpath()` + `dirname()` 定位 device 容器，普通目录导致 `dirname()` 返回错误路径，`uevent` 文件读取失败。

evdev minor 号必须从 64 起（`EVDEV_MINOR_BASE`）。sysfs 写 `13:64` 而设备节点是 `13:1`，libinput 的 `fstat().st_rdev` 反查失败。

`/run/udev/data/c<major>:<minor>` 文件必须存在。libudev 的 `get_is_initialized()` 仅检查文件存在性，无 udevd 时需手动 `touch` 空文件。

这三项都不是系统调用层面的问题。它们是 Linux 桌面生态 15 年演化的产物，散布在 udev 规则和用户态库源码中，没有任何集中文档记载。

## 4.4 两类应用的对比

| 维度 | PostgreSQL | Weston |
|------|-----------|--------|
| 核心依赖 | 进程模型、凭证、VFS、IPC、epoll | 驱动节点、DRM/KMS、evdev、netlink、AF_UNIX |
| 失败模式 | 明确错误码 + 日志，定位直接 | 多层级初始化失败，根因深埋 5+ 层调用栈 |
| 修复策略 | 按 syscall 逐个修复，PR 独立可合入 | 按子系统分层推进，PR 间有依赖关系 |
| 验证方式 | SQL 查询结果比对 | 图形输出 + evdev 事件校验 |
| PR 数量 | 16 个（含与实验2 共享的 syscall 修复） | 8 个（Wayland 路径专属） |
| 发现方式 | 启动日志直接指出失败的系统调用 | 阅读 libudev/libinput/libdrm 源码追踪判断条件 |

两者的适配过程共同揭示了一个核心事实: 真实应用在 StarryOS 上失败，原因极少是整系统调用缺失。绝大多数情况是系统调用存在、基本路径可通，但某个标志位组合、某个 errno 条件、某个 procfs/sysfs 节点格式、某个 ioctl 的返回值格式与 Linux 存在毫厘之差。PostgreSQL 需要约 10 个这样的修复，Weston 需要约 20 个。
