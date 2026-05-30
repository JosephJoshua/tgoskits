# 最终答辩幻灯片大纲

**作者**: Joseph Joshua (jsph273)
**日期**: 2026-05-30
**时长**: 20–30 分钟，约 45 页

---

## §1 封面 + 工作概览（2 页）

### Slide 1. 封面

- 标题: StarryOS 内核开发与 AI 辅助开发流水线
- 副标题: BigLab B 最终答辩
- 作者、日期、仓库: `rcore-os/tgoskits`

### Slide 2: 工作总览

```
AI 流水线 (starry-harness) ──→ 32 个 PR ──→ 两个大型应用运行
     9 技能 + 3 代理              11 独立 syscall 修复    PostgreSQL 启动
     16 脚本                      13 PG 路径发现          Weston 14 渲染
                                  8 Wayland 专属
时间线: 2026-04-15 → 05-21，六周
```

核心命题: 使 StarryOS 的 Linux syscall 语义从"基本可用"达到"精确对齐"。

---

## §2 实验1: AI 开发流水线（4 页）

### Slide 3: 动机: 三个痛点

- 测试反馈循环 3–5 分钟（build → rootfs → QEMU → test）
- Linux 对照成本高（手动编译、手动运行、手动读 man page）
- 重复性工作线性增长（每个 bug 的修复流程高度相似，状态追踪和报告维护负担随修复数量增加）

`starry-harness` 将这三类重复工作自动化。开发者保留架构决策与语义判断的控制权。

### Slide 4: 流水线架构

```
                    ┌─ Skills (9) ─────────────────────┐
                    │  hunt-bugs     测试→修复 主循环     │
                    │  test-app      Linux 应用兼容性测试  │
   User ──→         │  benchmark     性能基准             │
   triggers         │  audit-kernel  内核内部审计          │
   skills           │  review-quality 代码质量门禁         │
                    │  check-upstream 上游 PR 去重         │
                    │  start-submission 准备 PR 提交       │
                    │  evolve        自主持续开发循环       │
                    │  report        结构化报告生成         │
                    └────────────────────────────────────┘
                    ┌─ Agents (3) ──────────────────────┐
                    │  linux-comparator  Starry vs Linux │
                    │  kernel-reviewer   正确性/安全/并发  │
                    │  bug-triager       分类/严重性/去重  │
                    └────────────────────────────────────┘
                    ┌─ Scripts (16) ────────────────────┐
                    │  abi-check.py         syscall 参数数量 vs Linux 对照
                    │  lock-order-graph.py  静态锁顺序环检测
                    │  pattern-scanner.py   跨子系统缺陷模式扫描
                    │  kernel-graph.py      syscall→子系统 知识图谱
                    │  rust_analyzer.py     tree-sitter AST 解析（共享模块）
                    │  linux-ref-test.sh    Docker Linux 参照测试
                    │  strace-profiler.sh   应用 syscall 序列捕获
                    │  man-lookup.sh        man page 查询
                    │  regression-check.sh  回归检测
                    │  stress-test.sh       压力测试
                    │  change-tracker.py    PR 变更追踪
                    │  convert-test.py      测试格式转换
                    │  pipeline.sh          全流水线编排
                    │  draft-pr.sh          PR 描述生成
                    │  journal-entry.sh     日志追加
                    │  update-known.sh      known.json 状态更新
                    └────────────────────────────────────┘
```

关键设计原则: 脚本是确定性的（同一输入永远同一输出，不依赖 LLM）；代理处理需要判断的工作（对比行为、审查代码、分类 bug）。

### Slide 5: 核心工作流: hunt-bugs 五阶段

```
Phase 1: 发现                 Phase 2: 测试               Phase 3: 对比
  known.json 选目标      →    编写 C 测试程序        →    linux-comparator agent
  man-lookup.sh 查文档        交叉编译 riscv64/          StarryOS vs Linux
                              aarch64/x86_64            逐字段对比输出差异
                              linux-ref-test.sh
                              Docker Linux 参照

Phase 4: 修复                 Phase 5: 报告
  阅读 StarryOS 源码      →    report skill 生成报告
  kernel-graph.py 查依赖      update-known.sh 更新状态
  lock-order-graph.py          journal-entry.sh 追加日志
  检查死锁风险                 draft-pr.sh 准备 PR 描述
  验证: FAIL → PASS
```

### Slide 6: 三道工程护栏

| 护栏 | 机制 | 效果 |
|------|------|------|
| 结构化输出 | 所有 Agent 输出经 JSON Schema 验证 | 下游可无歧义消费，状态变化可追踪 |
| 状态记忆 | `known.json` 为所有缺陷的单一事实来源 | 防止重复修复、重复提交 |
| 审查否决权 | kernel-reviewer 独立运行，只找问题不批准 | 不合格的修复退回重改，直至全部维度通过 |

审查维度: 行为精确匹配 Linux？TOCTOU / UAF / 未初始化内存？锁顺序一致？死锁可能？错误路径回滚？回归测试覆盖所有触发条件？

---

## §3 实验2: 系统调用修复（8 页）

### Slide 7: 概述与 PR 总表

11 个独立 PR（排除通过 PostgreSQL / Wayland 发现的修复），覆盖 8 个子系统:

| PR | 日期 | 标题 | 子系统 |
|---|---|---|---|
| #197 | 04-15 | fix preadv/pwritev/preadv2/pwritev2 ABI | Net/FS |
| #205 | 05-07 | feat: implement mremap | Memory |
| #207 | 04-25 | fix: accept sigaltstack with exactly MINSIGSTKSZ | Signal |
| #208 | 04-19 | fix getgroups size=0 and clock_gettime invalid clock_id | Task/Timer |
| #209 | 04-30 | fix: negative value checks to ftruncate and pwrite64 | FS |
| #210 | 04-23 | fix: validate getrandom flags per Linux semantics | Net |
| #211 | 04-24 | fix: validate copy_file_range flags, types, and overlap | FS |
| #250 | 04-18 | fix: epoll_pwait sigsetsize with musl | Signal/epoll |
| #318 | 04-23 | fix: align loongarch64 smoke QEMU memory | Arch |
| #503 | 05-16 | feat: timerfd_create/settime/gettime | Timer |
| #505 | 05-16 | feat: dump user register state on fatal signals | Signal |

以下深挖三个代表性 PR: timerfd（fd 抽象层）、preadv/pwritev（syscall ABI 层）、mremap（内存管理层）。

### Slide 8: Deep Dive: timerfd (#503): 动机与设计

timerfd 是 Linux 特有的文件描述符类型，将 POSIX 定时器暴露为可被 epoll/select/poll 监听的文件描述符。`read()` 返回自上次读取以来的到期次数。systemd、dbus、Wayland event loop 均依赖此机制。

此 PR 从零实现一个完整 `FileLike` 类型，展示了 StarryOS 中新增 fd 的标准模式:

```
timerfd_create(clockid, flags) → fd
    └─→ 创建 TimerFd { clock, interval, expiration, is_oneshot }
    └─→ 注册到 FD_TABLE（通过 FileLike trait）

timerfd_settime(fd, flags, new_value, old_value)
    └─→ itimerspec: {it_interval, it_value}  周期与首次到期
    └─→ 标志: TFD_TIMER_ABSTIME（绝对/相对时间）
    └─→ 更新底层 axtask 定时器

read(fd) → u64 (到期次数)
    └─→ 非阻塞模式空队列返回 EAGAIN
    └─→ 读取后清零内部计数器
```

### Slide 9: timerfd (#503): 实现要点

`FileLike` trait 的完整实现:

- `read()`: 阻塞或非阻塞等待定时器到期，返回 `u64` 到期次数。非阻塞模式空队列返回 `EAGAIN`
- `write()`: 不支持，返回 `EINVAL`
- `close()`: 取消底层定时器，释放资源
- `poll()`: 查询定时器到期状态，支持 epoll 注册。到期时 waker 被唤醒，epoll 报告 `EPOLLIN`

关键细节:

- `itimerspec.it_interval = {0, 0}`: 一次性定时器，到期后不复用
- `itimerspec.it_interval > 0`: 周期性定时器，到期后自动重新武装
- 时钟源: `CLOCK_REALTIME`（墙上时间）、`CLOCK_MONOTONIC`（单调时间，不受系统时间调整影响）
- epoll 集成: `FileLike::poll()` 返回 `EPOLLIN`，定时器到期唤醒 epoll_wait

### Slide 10: Deep Dive: preadv/pwritev ABI (#197)

`preadv`/`pwritev` 的 64 位文件偏移在 32 位寄存器 ABI 中被拆分为两个参数:

```
sys_preadv(fd, iov, iovcnt, pos_l, pos_h)
                          └───┬───┘
                          64-bit pos 拆分
```

Linux 内核定义:
- `preadv`/`pwritev` → `SYSCALL_DEFINE5` (fd, vec, vlen, pos_l, pos_h)
- `preadv2`/`pwritev2` → `SYSCALL_DEFINE6` (fd, vec, vlen, pos_l, pos_h, flags)

StarryOS 此前 `preadv`/`pwritev` 仅读取 4 个参数（遗漏 `pos_h`），`preadv2`/`pwritev2` 仅读取 5 个参数（遗漏 `flags`）。遗漏的参数读到的是用户态栈上的残留值。

更隐蔽的问题: `sys_pwritev2` 的实现从 `sys_preadv2` 复制而来，内部调用仍为 `.read_at()` 而非 `.write_at()`。数据经 VFS 层传递后被当作读操作处理，写入内容全部丢失。此为 copy-paste 产生实现错误的典型案例。

修复: 重组 64 位偏移 `((pos_h as u64) << 32) | (pos_l as u64)`；修正 `pwritev2` 的内部调用方向。此 PR 后新增 `abi-check.py` 脚本，自动比对 StarryOS 每个 syscall 的参数数量与 Linux `SYSCALL_DEFINEn` 宏的元数。

### Slide 11: Deep Dive: mremap (#205): 动机

`mremap` 在虚拟地址空间中重新映射已有的内存区域。glibc 的 `realloc` 对大块内存使用 `mremap` 而非 `malloc` + `memcpy` + `free`。

三种模式:

| 模式 | 标志 | 行为 |
|------|------|------|
| 原地扩展 | 无 `MREMAP_MAYMOVE` | 尝试扩大 VMA，地址不变；失败返回 `ENOMEM` |
| 可移动扩展 | `MREMAP_MAYMOVE` | 当前地址不够时寻找新地址，更新页表映射 |
| 固定地址 | `MREMAP_FIXED` | 指定目标地址，替换已有映射（类似 `mmap(MAP_FIXED)`） |

此 PR 是理解"内核如何管理进程地址空间"的入口: 涉及 VMA 操作、页表更新、地址对齐约束。

### Slide 12: mremap (#205): 实现要点

核心流程:

```
mremap(old_addr, old_size, new_size, flags, new_addr)
  1. 验证 old_addr 页对齐，old_size > 0
  2. 在 AddrSpace 中查找 old_addr 对应的 VMA
  3. 检查 VMA 是否精确匹配 old_size
  4. 三种路径:
     a. new_size <= old_size: 收缩 VMA（释放尾部页面）
     b. new_size > old_size + 原地可扩展: 直接扩大 VMA
     c. MREMAP_MAYMOVE: 寻找空闲地址范围，复制页表映射
  5. 更新 VMA 集合与页表
```

关键约束:

- 新地址范围必须完全空闲（不可部分重叠已有映射）
- `MREMAP_FIXED` 会替换目标地址上的已有映射
- 收缩操作: 先 `munmap` 尾部页面，再更新 VMA 的 size 字段
- 所有操作在 `AddrSpace` 锁保护下原子完成

与 `mmap`、`munmap`、`brk` 共同构成 StarryOS 虚拟内存管理的四个基础原语。

### Slide 13: 共同模式

11 个 PR 中反复出现三类缺陷模式:

| 模式 | 案例 | 特征 |
|------|------|------|
| 遗留实现与新语义混淆 | #197 preadv/pwritev | 从早期简化实现演进时遗漏参数，copy-paste 未核验 |
| 直觉驱动的边界处理 | #207 sigaltstack (`<=` vs `<`), #250 epoll sigsetsize (严格 8 vs 接受 16) | Linux 的精确边界是 20 年 bug 修复的结果，非直觉可替代 |
| 参数校验的静默缺失 | #208 getgroups/clock_gettime, #209 ftruncate, #210 getrandom, #211 copy_file_range | 基本路径可通，非法参数不返回 EINVAL，调用方误以为操作成功 |

### Slide 14: 从独立修复到大型应用

这些独立修复使 StarryOS 通过大部分单次 syscall 测试。但真实应用的复杂性远超单次测试, PostgreSQL 和 Weston 在启动过程中调用数百个 syscall，任何一个环节的偏差都导致启动失败。

→ 过渡到 §4

---

## §4A 实验4: PostgreSQL 适配（9 页）

### Slide 15: 为什么选择 PostgreSQL

PostgreSQL 在常见 Linux 应用中属于少数同时深度依赖进程模型、进程间通信、文件系统语义、凭证系统、信号处理、事件循环的类别。Web 服务器主要压力在 epoll 与 sendfile，键值存储主要压力在 epoll 与内存分配。PostgreSQL 启动过程依次经历: setresuid 切换用户身份、AF_UNIX socketpair 建立进程间通道、SysV shmget 分配共享内存、epoll 配合 self-pipe 构造事件循环、fsync 作用于目录 fd 以确保元数据持久化、rename 与 fsync 组合实现原子持久化写入。

PostgreSQL 的关键优势: 启动失败时每个步骤输出精确的错误码和系统调用名。它的启动日志本身就是最好的测试框架，不需要 strace。

### Slide 16: PostgreSQL 进程模型

PostgreSQL 采用每连接独立进程模型（非线程池）:

```
postmaster (主进程, 以 postgres 用户运行)
  │
  ├─ fork() ──→ startup process   (WAL 恢复, 一次性)
  ├─ fork() ──→ checkpointer      (脏页刷写)
  ├─ fork() ──→ bgwriter          (后台写入)
  ├─ fork() ──→ walwriter         (WAL 日志刷写)
  ├─ fork() ──→ autovacuum launcher (按需触发 worker)
  ├─ fork() ──→ archiver          (WAL 归档, 可选)
  ├─ fork() ──→ stats collector   (统计信息收集)
  └─ fork() ──→ backend × N       (每客户端连接 fork 一个)
```

每个 fork 子进程的初始化序列:
1. `setresuid/setresgid` → 切换至 postgres 用户（uid=70）
2. `prctl(PR_SET_PDEATHSIG, SIGTERM)` → 父进程终止时收到 SIGTERM
3. `prlimit64(RLIMIT_NOFILE, ...)` → 提高文件描述符限制
4. `getrlimit(RLIMIT_STACK)` → 计算 max_stack_depth

任一步骤失效均导致 worker 退出，postmaster 判定系统不稳定并中止启动。

### Slide 17: 进程间通信层

PostgreSQL 使用三条独立的 IPC 通道:

**SysV 共享内存（shared_buffers 页缓存层）:**
```
postmaster: shmget(IPC_PRIVATE, size, IPC_CREAT | 0600)
backend:    shmat(shmid, NULL, 0)   // fork 后继承映射
            shmdt(shmaddr)          // 进程退出时分离
```
要求: 多进程并发 attach/detach 不可死锁；`IPC_RMID` 在 nattch==0 时立即销毁；`clear_proc_shm` 须调用 `aspace.unmap()`（物理页已释放但映射仍在则构成 use-after-free）。

**AF_UNIX socketpair（postmaster↔worker 控制通道）:**
```
postmaster: socketpair(AF_UNIX, SOCK_STREAM) → {sock0, sock1}
            fork()  // 子进程继承 sock1
worker:     close(sock1)  // 写端关闭
postmaster: recv(sock0) → 期待返回 0 (EOF)
```
要求: 写端关闭后读端必须返回 0 (EOF) 而非 EAGAIN；`bind()` 创建的 socket 文件属主为调用者凭证；`chmod()` 允许非 root 调用者修改自己创建的 socket 权限。

**Self-pipe + epoll（WaitEventSet 事件循环）:**
```
pipe2(self_pipe, O_NONBLOCK) → {rfd, wfd}
epoll_create1(EPOLL_CLOEXEC) → epfd
epoll_ctl(epfd, EPOLL_CTL_ADD, rfd, EPOLLIN)
epoll_ctl(epfd, EPOLL_CTL_MOD, latch_fd, EPOLLIN)  // 反复重武装
epoll_wait(epfd, events, maxevents, -1)
```
要求: EPOLL_CTL_MOD 后事件不可丢失；LT 模式不可对同一就绪 fd 返回 N 份副本；consume() 与 register() 之间不可有时间窗口。

### Slide 18: 存储层与完整启动序列

PostgreSQL 的持久化写入采用 durable_rename 模式:

```
write(tmp_file)          // 写入临时文件
fsync(tmp_file)          // 确保数据到达存储介质
rename(tmp_file, final)  // 原子重命名为最终路径
fsync(parent_dir_fd)     // 确保目录项持久化（POSIX 要求）
```

完整启动序列（标注 StarryOS 上曾中断的位置）:

```
[postmaster 启动]
 1. setresuid/setresgid → uid=70                    ← 空操作, 无效果
 2. socketpair(AF_UNIX, SOCK_STREAM)
 3. bind(AF_UNIX, "/tmp/.s.PGSQL.5432")
 4. chmod("/tmp/.s.PGSQL.5432", 0777)               ← EPERM (属主错误)
 5. shmget(IPC_PRIVATE, size, IPC_CREAT|0600)       ← SMP 死锁
 6. shmat(shmid, NULL, 0) × N                       ← SMP 死锁
 7. epoll_ctl(epfd, EPOLL_CTL_MOD, latch_fd)        ← 事件丢失
 8. epoll_wait(epfd, events, maxevents, -1)          ← LT 重复事件
 9. fork() → 子进程
10. prctl(PR_SET_PDEATHSIG, SIGTERM)                 ← 被静默忽略
11. prlimit64(RLIMIT_NOFILE, ...)                    ← 返回成功但不生效
12. getrlimit(RLIMIT_STACK)                          ← 默认 512K (应为 8M)

[initdb]
13. fsync(data_dir_fd)                               ← 返回 EISDIR
14. rename(pg_control_tmp, pg_control)               ← 目标文件内容清零
15. fchown(pg_control, uid=70)                       ← 凭证缺失

[运行查询]
16. accept() → SIGUSR1 → SA_RESTART → accept()       ← 返回 EINTR 而非重启
17. 并行查询 → interrupt waker                       ← 竞态, 永久挂起
```

### Slide 19: 修复: 凭证子系统 (#246) + SA_RESTART (#247)

**凭证子系统 (#246)。** 整个适配中工程量最大的单项修改。

```
struct Cred {
    uid, gid       // 真实身份
    euid, egid     // 有效身份（权限检查用）
    suid, sgid     // 保存的 set-user-ID
    fsuid, fsgid   // 文件系统访问专用
    sup_groups[]   // 补充组列表
}
```

`Arc<Cred>` 引用计数共享，fork 时继承。接入点: `faccessat2` 基于 fsuid 检查；`fchownat` 需 CAP_CHOWN 或文件属于调用者；`kill` 检查发送者与目标的 uid/euid/suid 匹配；IPC 子系统的所有权与权限检查；`/proc/<pid>/status` 的 Uid/Gid 字段。

`setresuid(-1, 1000, -1)` 的 NOCHG 语义: -1 表示该字段不修改。几乎所有 Linux daemon 依赖此细节。

**SA_RESTART (#247)。** 信号处理与系统调用交互的完整案例。

RISC-V/AArch64 上 a0/x0 的寄存器复用问题: 系统调用进入时 a0 为首参数（如 fd=3）；信号处理后内核将 `-EINTR` 写入 a0；SA_RESTART 要求恢复 a0 为 3。修复三步: PC 回退 `SYSCALL_INSN_LEN` 字节（RISC-V/AArch64=4, x86_64=2）；恢复 a0 为原始参数；不调用 `set_retval(0)`（在 RISC-V/AArch64 上会覆盖刚恢复的 a0）。

### Slide 20: 修复: 并发正确性问题 (#226, #504, #316)

**IPC 共享内存死锁 (#226)。** SMP=4 条件下 95% 复现率:

```
sys_shmget: SHM_MANAGER → shm_inner
sys_shmat:  shm_inner → aspace → SHM_MANAGER
                  ↑_______________↑
                   锁顺序相反 → AB/BA 死锁
```

修复统一全局锁顺序为 SHM_MANAGER → shm_inner → aspace。`sys_shmdt` 分阶段加锁: 查 shmid（持 SHM_MANAGER 后释放）→ 读 va_range（持 shm_inner 后释放）→ 解除映射（仅持 aspace）→ 更新 bookkeeping（重新获取）。同步修复 `clear_proc_shm` 缺失 `aspace.unmap()` 导致的 use-after-free，以及 `IPC_RMID` 在 nattch==0 时未立即销毁内存段。

**epoll LT 就绪队列 (#504)。** 电平触发模式下 `epoll_wait(maxevents=N)` 对单个就绪 fd 返回 N 份副本:

```
bug: pop_front → push_back 循环 → 同一 interest 被消费 maxevents 次
fix: mem::take drain → 每 interest 仅访问一次 → LT 幸存者放 keep → splice 回
```

同步修复 `consume()` 与 `register()` 之间的事件丢失窗口，采用 register-then-check 模式。

**Interrupt waker 竞态 (#316)。** swap(false) 与 register(waker) 之间存在窗口: 在此窗口内另一任务调用 interrupt()，发现 waker 槽为空，wake 被丢弃。修复将 register 移到 swap 之前, register-then-check 模式在本次项目中反复出现（SA_RESTART、waitpid、interrupt waker、epoll check_and_register_waker）。

### Slide 21: 修复: VFS rename (#312) + EPOLL_CTL_MOD (#314)

**VFS rename 原子性 (#312)。** tmpfs 上 `rename(src, dst)` 后目标文件内容全零:

```
DirNode::rename(src, dst):
  bug: forget_entry(src) → DirEntry Arc→0 → page cache 释放
       lookup(dst) → 新 DirEntry → 空 page cache
  fix: remove_entry(src) 取出而非 forget
       insert_entry(dst, same_DirEntry) → page cache 保持
```

rename 的语义是重关联同一文件对象至新名称，复制内容后删除旧路径。对 tmpfs 此类纯内存文件系统，维持此语义的唯一途径是保持 DirEntry 实例不变, DirEntry 一旦变更，page cache 即断裂。

**EPOLL_CTL_MOD 事件丢失 (#314)。** PostgreSQL 的 WaitEventSet 对 latch fd 反复执行 MOD 重武装后 `epoll_wait` 收不到事件:

```
Epoll::modify:
  1. 替换 interests[fd] 中的 Arc<EpollInterest> (旧→新)
  2. 旧 Arc 引用计数归零 → 释放
  3. ready_queue 中悬挂的 Weak<旧 Arc> → upgrade() → None → 静默跳过
  4. 新 Arc 无路径进入 ready_queue → epoll_wait 永远看不到此事件
```

修复: 替换前记录旧 Arc 的 `is_in_queue()` 状态；若旧条目已在 ready_queue 中，立即将新 Arc 的 Weak 推入 ready_queue。

### Slide 22: PostgreSQL 适配总结

13 个 PR，按问题层次分类:

| 层次 | PR 数 | 代表 | 特征 |
|------|-------|------|------|
| 缺失功能 | 1 | #246 凭证 | 整套子系统从头构建 |
| 语义偏差 | 9 | #251 fsync 目录, #312 rename, #314 CTL_MOD, #504 LT, #247 SA_RESTART, #311 AF_UNIX EOF, #313 socket chown, #319 prlimit64, #248 RLIMIT_STACK, #249 PDEATHSIG | 基本路径正确，标志位/errno/边界与 Linux 不一致 |
| 并发正确性 | 3 | #226 SHM 死锁, #316 waker 竞态, #504 epoll 窗口 | 单核正确，SMP 暴露 |

13 个 PR 所修复的均为通用内核缺陷。凭证子系统影响所有使用 setresuid 的应用；epoll LT 修复影响 libuv、Go netpoll、nginx；VFS rename 修复影响 SQLite 及各类包管理器。PostgreSQL 是首个系统性暴露这些问题的应用。

---

## §4B 实验4: Wayland/Weston 适配（12 页）

### Slide 23: 为什么选择 Weston

PostgreSQL 测试的是 POSIX 模型（进程、凭证、IPC、文件系统）。Weston 测试的是 Linux 驱动模型和用户态图形生态。两者覆盖了 StarryOS 内核接口的互补侧面。

| 维度 | PostgreSQL | Weston |
|------|-----------|--------|
| 核心依赖 | 进程模型、凭证、VFS、IPC、epoll | 驱动节点、DRM/KMS、evdev、netlink、AF_UNIX SCM_RIGHTS |
| 测试的内核层 | syscall 语义层 | 驱动 ioctl + sysfs + udev 契约层 |
| 失败模式 | 明确错误码 + 日志 | 多层级初始化失败，根因深埋 5+ 层调用栈 |
| 发现方式 | 启动日志直接指出失败的系统调用 | 阅读 libudev/libinput/libdrm 源码追踪判断条件 |

Weston 的启动路径穿透了整个 Linux 用户态图形栈:

```
Weston (合成器进程)
  ├─→ drm-backend      ← libdrm → /dev/dri/card0 → DRM/KMS
  ├─→ libinput         ← libudev → /sys, /dev/input/eventN → evdev
  ├─→ wayland-server   ← AF_UNIX socket → SCM_RIGHTS fd 传递
  └─→ event loop       ← epoll + timerfd
```

### Slide 24: 子系统 1: DRM/KMS 图形栈（资源枚举）

DRM 是 Linux 用户态图形的基础设施。Weston 通过 libdrm 与它交互。

**设备打开与能力协商:**
```
drmOpen("/dev/dri/card0") → fd
drmGetVersion(fd) → name, version, date
drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES)
drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC)
```
libdrm 会盲探约十种 capability。内核必须对未知 cap 返回 0（而非报错）, libdrm 以此探测内核支持的特性集。

**KMS 资源枚举:**
```
drmModeGetResources(fd)
  ├─→ CRTCs 列表        ← 显示控制器
  ├─→ Connectors 列表    ← 物理输出端口
  ├─→ Encoders 列表      ← CRTC↔Connector 信号编码器
  └─→ framebuffers 列表  ← 已注册的 framebuffer 对象

drmModeGetConnector(fd, conn_id) → 显示模式列表 (drmModeModeInfo)
drmModeGetPlane(fd, plane_id) → 像素格式 (XRGB8888...) + 类型 (Primary/Cursor/Overlay)
```

**属性系统:**
```
drmModeObjectGetProperties(fd, CRTC_ID, DRM_MODE_OBJECT_CRTC)
  ├─→ "ACTIVE"  : RANGE [0, 1]      ← weston 硬编码检查此类型
  └─→ "MODE_ID" : BLOB              ← 通过 Property Blob 回环

drmModeObjectGetProperties(fd, PLANE_ID, DRM_MODE_OBJECT_PLANE)
  ├─→ "type"    : ENUM {Primary, Cursor, Overlay}
  ├─→ "FB_ID"   : OBJECT (framebuffer)
  ├─→ "CRTC_ID" : OBJECT (CRTC)
  └─→ "SRC_X/Y/W/H", "CRTC_X/Y/W/H" : 源/目标矩形
```

关键约束: `CRTC.ACTIVE` 类型必须是 `RANGE [0, 1]`。如果是 ENUM 或 SIGNED_RANGE，weston 的 atomic backend 直接拒绝。

### Slide 25: 子系统 1: DRM/KMS 图形栈（模式设置）

**Framebuffer 分配与原子提交:**
```
drmModeCreateDumb(fd, w, h, bpp) → {handle, size, pitch}
drmModeAddFB2(fd, w, h, format, handles[], pitches[], offsets[], &fb_id, 0)

drmModeCreatePropertyBlob(fd, &mode, sizeof(mode)) → blob_id
drmModeAtomicAlloc() → atomic_request

drmModeAtomicAddProperty(req, CRTC_ID, "ACTIVE", 1)
drmModeAtomicAddProperty(req, CRTC_ID, "MODE_ID", blob_id)
drmModeAtomicAddProperty(req, PLANE_ID, "FB_ID", fb_id)
drmModeAtomicAddProperty(req, PLANE_ID, "CRTC_ID", crtc_id)
drmModeAtomicAddProperty(req, PLANE_ID, "SRC_W", width << 16)   // .16.16 定点格式
drmModeAtomicAddProperty(req, PLANE_ID, "SRC_H", height << 16)
drmModeAtomicAddProperty(req, PLANE_ID, "CRTC_W", width)
drmModeAtomicAddProperty(req, PLANE_ID, "CRTC_H", height)

drmModeAtomicCommit(req, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT)
```

**MODE_ID blob 回环:** 用户态通过 `CreatePropertyBlob` 将 `drmModeModeInfo` 序列化为 blob 并获取 ID；内核原样保存 blob 内容；用户态通过 `GetPropertyBlob` 读回完全一致的字节序列；commit 后通过 `DestroyPropertyBlob` 销毁。

**PAGE_FLIP 事件循环:** commit(PAGE_FLIP_EVENT) → 硬件完成翻页 → 内核向 DRM fd 写入 `drm_event_vblank` → epoll 唤醒 weston → `drmHandleEvent()` 读取事件 → 提交下一帧。

Mode list 使用 CVT-RBv1 时序参数合成。须用真实时序, 随便填的数字会被 weston mode validator 拒绝（水平/垂直总像素、前后肩、同步脉冲宽度需自洽）。

### Slide 26: 子系统 2: evdev 输入 + libinput

libinput 是 Weston 的输入后端，通过 libudev 发现设备、通过 evdev ioctl 查询能力。

**设备发现:**
```
udev_new() → udev_enumerate_new()
udev_enumerate_add_match_subsystem("input")
udev_enumerate_scan_devices()
  ├─→ 遍历 /sys/class/input/
  │   └─→ realpath() 解析软链 → dirname() 定位 device 容器 → 读 uevent
  ├─→ 遍历 /sys/dev/char/<major>:<minor> → 建立设备号↔syspath 反向映射
  └─→ 检查 /run/udev/data/c<major>:<minor> → 判定设备是否已 udev 初始化
```

**设备能力查询:**
```
open("/dev/input/event0")
ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), &bits)  → 按键能力位图
ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), &bits)   → 绝对轴能力
ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), &bits)   → 相对轴能力
ioctl(fd, EVIOCGPROP(0), &prop_bits)               → 设备属性位
ioctl(fd, EVIOCGABS(ABS_X), &abs_info)             → {min, max, fuzz, flat, resolution}
```

**libinput 的分类决策树:**
```
INPUT_PROP_POINTER + INPUT_PROP_BUTTONPAD → 触控板
INPUT_PROP_POINTER (无 BUTTONPAD)         → 鼠标
INPUT_PROP_DIRECT + ABS_X + ABS_Y        → 触屏
无属性 + REL_X/REL_Y                      → 传统鼠标
无属性 + 按键                              → 键盘
```

缺失任何一个 ioctl 返回值，设备无法被正确分类，libinput 将其事件当作噪声丢弃。

### Slide 27: 子系统 3: Wayland 协议与 FD 传递

**Wayland 协议的 IPC 机制:**
```
[客户端]
memfd_create("wayland-shm", MFD_ALLOW_SEALING) → shm_fd
ftruncate(shm_fd, w * h * 4)
mmap(NULL, size, PROT_WRITE, MAP_SHARED, shm_fd, 0) → 像素 buffer
write(pixels)
sendmsg(sock, {SCM_RIGHTS, [shm_fd]})  // 传递 fd 给合成器

[合成器 (Weston)]
recvmsg(sock, MSG_DONTWAIT, {SCM_RIGHTS}) → 收到 shm_fd
mmap(NULL, size, PROT_READ, MAP_SHARED, shm_fd, 0) → 读取像素
```

**SCM_RIGHTS 在 SOCK_STREAM 上的 byte-mark 语义:**

这是 Wayland 协议对 AF_UNIX 最严格的要求。一次 `sendmsg` 调用中，cmsg 携带的 fd 与同一调用写出的第一个字节原子交付。接收端 `recvmsg` 在读到此 fd 对应的字节范围的首字节时同时获得 fd。

Linux 实现: `unix_stream_sendmsg` 将 `[start_byte, end_byte]` 范围与 `scm_fp_list` 绑定；`unix_stream_recvmsg` 截断读取在下一条 cmsg 的 `end_byte` 处。消息边界不匹配时客户端传的 fd 与合成器期望的 fd 错位，Wayland 协议判定违规并断开连接。

### Slide 28: 子系统 4: memfd 与 F_SEAL_*

memfd 是 Wayland 共享内存机制的内核基础:

```
memfd_create("wayland-shm", MFD_ALLOW_SEALING) → fd
ftruncate(fd, size)
fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK)   // 禁止缩小
fcntl(fd, F_ADD_SEALS, F_SEAL_GROW)     // 禁止扩大
fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE)    // 禁止写入
fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL)     // 不可逆锁定
```

Seal 语义渗透到三个独立内核入口:

| Seal | 影响的系统调用 | 行为 |
|------|-------------|------|
| `F_SEAL_SHRINK` | `ftruncate` new_size < old_size | 拒绝，返回 `EPERM` |
| `F_SEAL_GROW` | `ftruncate` new_size > old_size; `write` 跨 EOF | 拒绝扩大；write 被拒且文件大小回退 |
| `F_SEAL_WRITE` | `write`; `mmap(PROT_WRITE, MAP_SHARED)` | 拒绝写入；mmap 返回 `EPERM` |
| `F_SEAL_SEAL` | `fcntl(F_ADD_SEALS, ...)` | 拒绝所有后续 seal 修改 |

并发安全: `add_seals` 使用 `compare_exchange` 循环原子合并 seal 位；`set_len_sealed` 在持 `truncate_mtx` 状态下完成"读取长度→校验 seal→修改大小"三步。不带 `MFD_ALLOW_SEALING` 创建的 memfd 初始即设置 `F_SEAL_SEAL`, seal 集合永久锁定为空。

### Slide 29: 隐性契约: sysfs、udev 与设备发现

Weston 适配中最困难的部分。三个 bug 都不是 syscall 层面的问题:

**① sysfs class 目录必须是软链:**
```
Linux:    /sys/class/drm/card0 → /sys/devices/virtual/drm/card0 (软链)
StarryOS: /sys/class/drm/card0 (普通目录)

libudev 的操作:
  path = realpath("/sys/class/drm/card0")   // → "/sys/devices/virtual/drm/card0"
  container = dirname(path)                  // → "/sys/devices/virtual/drm"
  read(container + "/uevent")                // 读设备属性
```

普通目录下 `realpath()` 返回原路径，`dirname()` = `/sys/class/drm`，该目录下无 `uevent` 文件。修复将 class 目录改为指向 `/sys/devices/virtual/<subsystem>/...` 的软链。

**② evdev minor 号必须从 64 起:**
```
Linux: EVDEV_MINOR_BASE = 64
  /dev/input/event0 → major=13, minor=64

StarryOS 原: sysfs 写 "13:64" 但设备节点实际为 "13:1" (minor = i+1)

libinput: fstat("/dev/input/event0") → st_rdev = makedev(13, 1)
          扫描 /sys/dev/char/13:64/ → 找不到匹配 → 报错
```
修复将 minor 号从 `i+1` 改为 `64+i`。

**③ /run/udev/data/ 预填空文件:**
```
libudev: access("/run/udev/data/c13:64", F_OK) == 0 ? "initialized" : "skip"
StarryOS: 无 udevd → 文件不存在 → libinput 判定设备未初始化 → skip
```
修复: `touch /run/udev/data/c13:64 ...` 空文件。libudev 仅检查存在性，文件内容无关。

这三种契约散布在 udev 规则和用户态库源码中，没有任何集中文档记载。

### Slide 30: 修复: DRM/KMS 设备节点 (#506)

Weston 首个阻塞点: `drmOpen("/dev/dri/card0")` → `ENOENT`。

从零创建 DRM 设备节点，实现 legacy 与 atomic 两条 KMS 路径:

| 路径 | ioctl | 用途 |
|------|-------|------|
| Legacy | `SETCRTC` | 基本模式设置 |
| Legacy | `PAGE_FLIP` | 翻页通知 |
| Legacy | `WAIT_VBLANK` | 等待垂直消隐 |
| Atomic | `ATOMIC_COMMIT` | 原子提交 CRTC + Plane + Connector |
| Atomic | `ATOMIC_TEST_ONLY` | 测试模式 |
| 通用 | `GETRESOURCES`, `GETCONNECTOR`, `GETPLANE` | 资源枚举 |
| 通用 | `GETPROPERTY`, `GETPROPBLOB`, `CREATEPROPBLOB`, `DESTROYPROPBLOB` | 属性系统 |

关键细节: 未知 capability 返回 0（libdrm 探针）；`CRTC.ACTIVE` 类型为 `RANGE [0, 1]`（weston 硬编码检查）；MODE_ID blob 字节级回环；Mode list 用 CVT-RBv1 真实时序合成。

测试: 83 项断言覆盖 version / modeset / atomic 三条路径，4 架构 PASS。

### Slide 31: 修复: evdev 多设备与输入 ioctl (#513)

**设备分类误判。** `input_devices()` 发现带 `BTN_MOUSE` 位的设备时仅挂为 `/dev/input/mice`，不创建 eventN。QEMU virtio-keyboard 因 `BTN_MISC` 在同一字节报位，键盘误判为鼠标。后注册设备覆盖先注册的，event0 消失。修复: 每个设备均暴露为 `/dev/input/eventN`，首个鼠标 alias 到 mice。

**EVIOCGPROP 未实现。** libinput 通过此 ioctl 获知设备属性位（`INPUT_PROP_POINTER`、`INPUT_PROP_DIRECT`、`INPUT_PROP_BUTTONPAD`）。未实现时所有位为 0，libinput 判定"无 pointer 属性"→ 将触屏事件当作噪声丢弃。

**EVIOCGABS 未实现。** libinput 查询 `ABS_X.min/max`、`ABS_Y.min/max` 获取坐标范围。全零时触屏区域为 0×0，所有事件在边界外。

修复: `axdriver_input` trait 新增 `get_prop_bits()` 和 `get_abs_info()` 默认方法；virtio-input 驱动实现透传；StarryOS evdev 对带轴但无 `INPUT_PROP_DIRECT` 的设备合成 `INPUT_PROP_POINTER`。

### Slide 32: 修复: Weston bringup 综合修复包 (#509)

Wayland 路径上最复杂的单项修改，含 7 项互相依赖的修复:

| 编号 | 修复 | 问题 |
|------|------|------|
| N1 | mmap PROT_WRITE 自动补 PROT_READ | RISC-V 特权规范将 R=0 W=1 列为 reserved PTE |
| N2 | EventDev::register 不再无条件 wake | LT 下 register→wake→consume(empty)→register 死循环 ~500Hz |
| N3 | /dev/input 设备分类修正 | BTN_MISC 导致键盘误判为鼠标 |
| P1 | virtio-input IRQ 唤醒 | 仅靠 16ms tick 兜底，输入延迟 16ms |
| P2 | register_irq_waker 重构 | IRQ 上下文 SpinNoIrq 与普通任务上下文可能死锁 |
| P3 | EventDev 多事件缓冲（`VecDeque`, 容量 256） | 单槽位 read_ahead，burst 丢事件 |
| P4 | AF_UNIX SCM_RIGHTS + MSG_DONTWAIT | cmsg 被忽略，fd 凭空消失 |

**P4 实现:**
```
PendingCmsg { start_byte, end_byte, cmsg } 队列
send: [start_byte, end_byte] 与 cmsg 绑定入队
recv: 读取截断在下一条 cmsg 的 end_byte 处
      cmsg 与对应字节范围的首字节原子交付
```

register-then-check 模式在 IRQ waker 中再次应用: 先 `register(waker)`，再 `swap(false)` 检查。

### Slide 33: 修复: aarch64 架构支持 (#510, #511)

**#510: EL0 cache 指令使能。** aarch64 上 Weston 在 `__libc_start_main` 之前收到 `SIGTRAP`。

根因: `SCTLR_EL1` 寄存器三位未置位:

| 位 | 名称 | 控制 |
|----|------|------|
| bit 15 | UCT | EL0 访问 `CTR_EL0`（cache type register） |
| bit 14 | DZE | EL0 执行 `DC ZVA`（data cache zero by address） |
| bit 26 | UCI | EL0 执行 `DC CVAU` 和 `IC IVAU`（cache maintenance） |

musl/glibc 启动早期需读 `CTR_EL0` 获取 cache line size；Mesa 的 SIMD 路径需 `DC ZVA` 零拷贝。`init_mmu()` 此前仅设 MMU 和 cache 使能位，三个 EL0 位全为零。

**#511: GICv3 + CNTV for Apple HVF。** Apple Hypervisor Framework 下 GICv2 路径访问 GICC MMIO 时 HVF 无法提供 `ESR.ISV` → QEMU `assert(isv)` 失败。CNTP 被 EL2 占用。可用路径: GICv3 system-register CPU interface + CNTV virtual timer。修复增加 `gic-v3` 和 `cntv-timer` 两个 feature flag，`starryos/aarch64-hvf` 便捷 feature 同时启用。默认 QEMU TCG 与物理板保持 GICv2 + CNTP 路径不变。

### Slide 34: Weston 适配总结

8 个 PR，按子系统分层推进:

| 子系统层 | PR | 修复内容 |
|---------|-----|---------|
| 架构基础 | #510, #511 | aarch64 EL0 cache 指令、GICv3+CNTV |
| 设备节点 | #506 | /dev/dri/card0: DRM/KMS legacy+atomic 双路径 |
| sysfs/udev | #508 | class 软链、evdev minor=64、/run/udev/data/ 预填空 |
| 输入 | #513 | 每设备 eventN、EVIOCGPROP/EVIOCGABS |
| 图形协议 | #507 | memfd F_SEAL_SHRINK/GROW/WRITE/SEAL + 并发安全 |
| 网络 | #512 | NETLINK_ROUTE/GENERIC/KOBJECT_UEVENT + 合成 rtnetlink 应答 |
| 综合修复 | #509 | PROT_WRITE、IRQ waker、SCM_RIGHTS byte-mark、MSG_DONTWAIT |

内核图形栈的兼容性不是 checklist 式的功能对照。它是对 Linux 用户态生态中隐性契约的逐层发现。修复本身加起来不过数百行，但定位问题需要穿透五层抽象（Weston → libinput/libdrm → libudev → sysfs/evdev → 内核），每层都可能隐藏一个微小的格式偏差。

---

## §5 StarryOS 架构分析（5 页）

### Slide 35: 架构概览

StarryOS 在 ArceOS unikernel 的模块体系之上叠加一层 Linux syscall ABI 兼容层:

```
┌──────────────────────────────────────────────┐
│  应用层       │ Alpine Linux 用户空间程序      │
├──────────────────────────────────────────────┤
│  系统调用层   │ starry-kernel (~210 syscalls) │
│              │ 640 行巨型 match 分发器         │
├──────────────────────────────────────────────┤
│  组件层       │ starry-process / starry-signal │
│              │ starry-vm / starry-smoltcp     │
├──────────────────────────────────────────────┤
│  ArceOS 模块层│ axhal / axtask / axmm / axfs   │
│              │ axnet / axdriver / axalloc     │
├──────────────────────────────────────────────┤
│  平台抽象层   │ ax-plat (8 个板级实现)          │
│              │ riscv64/aarch64/x86/loong      │
└──────────────────────────────────────────────┘
```

核心设计: 复用 ArceOS 的硬件抽象、驱动框架、调度器、页表、文件系统，通过约 640 行的巨型 match 分发器实现约 210 个 Linux syscall 语义。StarryOS 代码量约 60% 在 syscall/ 及其子模块中。

### Slide 36: 有利于 AI 开发的架构特征

**① 分层模块化。** 修改 `starry-signal` 无须理解 ArceOS 的 virtio 驱动。`starry-process` 作为纯数据模型，不依赖调度器与 MMU，其单元测试在 no_std 内核环境中同样成立。AI 代理可仅阅读相关层源码，不必加载整个内核上下文。

**② 类型系统的编译时验证。** 四种内存映射后端通过 `enum_dispatch` 统一为 `Backend` 枚举，编译器强制穷举所有变体。新增变体时编译器自动标记所有未处理分支。

**③ 确定性的信号投递。** 信号在 syscall/异常/缺页返回后同步检查，非异步抢占。信号投递时机完全由 syscall 状态驱动，可复现、可测试。

**④ 资源作用域的可追踪性。** `scope_local` 机制将 FD 表和 FS 上下文绑定到进程作用域。每个 fd 必然属于某个活跃作用域，不存在悬空引用。

**⑤ Linux 行为预言机。** starry-harness 在 Docker Linux 上运行与 StarryOS 相同的测试输入，逐字段对比。测试框架本身编码了正确性标准，对 AI 开发减少了"什么是正确行为"的认知成本。

### Slide 37: 产生系统性摩擦的架构特征

**① 巨型 match 分发器。** 约 210 个 syscall 集中在一个函数中，syscall 之间没有模块边界。验证一个 syscall 修复需"构建内核 → 制作 rootfs → 启动 QEMU → 运行测试"，单次迭代 3–5 分钟。

**② 能力发现机制缺失。** 8 个 syscall 返回空操作的匿名 fd（`userfaultfd`、`io_uring_setup`、`memfd_secret` 等），调用成功但无实际功能。这是最危险的失败模式: 调用方无法通过返回值判断操作是否生效。对比: `fanotify_init` 返回 `-ENOSYS` 反而提供了明确信号。

**③ 层间隐式耦合。** `TaskExt::on_enter/on_leave` 通过 `scope_local` 隐式副作用传递进程状态，无显式返回值。调度器与进程管理器之间的契约由 Cargo 特性组合与运行时副作用定义，非类型系统可验证。

**④ 特性标志透明性缺失。** 三层特性级联（用户配置→StarryOS binary features→ax-feat 聚合），特性冲突不在编译时报错。`plat-dyn` 在 x86_64/loongarch64 上被静默禁用以回退到静态平台模式，与用户意图产生静默偏差。

**⑤ 凭证模型的语义偏差。** credentials 存储在 `Thread.cred`（per-thread），Linux 为 per-process。`set_cred()` 遍历所有线程同步凭证时存在时间窗口，同一进程的不同线程拥有不同 credentials。

### Slide 38: 执行 vs 决策: 六周数据

32 个 PR 的完成过程提供了关于 AI 辅助开发定位的具体数据:

| 维度 | AI 承担 | 人工承担 | 说明 |
|------|---------|---------|------|
| 编写代码 | ~70% | ~30% | 模板代码、测试生成、配置修改 |
| 生成测试 | ~80% | ~20% | C 测试程序自动化 |
| 运行 clippy/fmt | ~95% | ~5% | 完全自动化 |
| 判断根因 | ~10% | ~90% | 需理解子系统交互与语义 |
| 确定修复方向 | ~10% | ~90% | 在多个方案中选择 |
| 选择锁策略 | ~5% | ~95% | 传递性死锁检查不可靠 |
| 评估传递性影响 | ~5% | ~95% | 修改对全局状态的影响 |
| 架构决策 | 0% | 100% | 如凭证存储 per-thread vs per-process |

AI 承担了约 70% 的执行工作量，但约 90% 的决策工作量依赖人工判断。AI 的核心价值在于消除"已知问题所在但修改成本过高"的摩擦，而非替代"问题尚不明确需要探索"的认知过程。

### Slide 39: 重新设计: 六个方向

若重新设计一个面向 AI 辅助开发友好的内核，核心思路是使架构内建更多"变更可被验证"的机制:

| 改进 | 当前 | 设计目标 |
|------|------|---------|
| 可插拔 syscall 注册表 | 640 行 match，无模块边界 | 每个 syscall 声明编号+签名+返回类型，注册到可查询表。单个 syscall 可独立编译、测试、fuzz |
| 显式能力矩阵 | 8 个 syscall 返回静默成功的 dummy fd | `/proc/sys/kernel/capabilities` 暴露每个功能的实现状态（full/stub/missing） |
| 层级间形式化契约 | TaskExt 依赖隐式副作用 | `on_enter() → ActiveScope`、`on_leave(scope)` 显式返回值。关键 invariants 用 `debug_assert!` 验证 |
| 规约驱动开发 | 无机器可读 syscall 语义规约 | YAML/TOML 格式规约（参数约束、返回值语义、错误条件、副作用），驱动测试生成和 fuzz |
| 自动化差异测试 | Linux 对照需手动操作 | 每个 PR 自动在 Docker Linux 上运行相同输入，差异分类为已知偏差/待定/回归 |
| 验证基础设施统一 | 上游 CI 与 monorepo CI 测试集不重叠 | 一次 CI 运行覆盖所有相关测试，测试覆盖变化可追踪 |

方向是在现有层次之间插入显式的契约和验证点，而非推倒重来。

---

## §6 反思 + OS 课程建议（3 页）

### Slide 40: 对兼容性的重新理解

六周的工作改变了对"操作系统兼容性"的理解。

此前认为兼容性即实现 syscall 表: 对照 Linux 的 syscall 编号列表逐一实现，跑通即完成。实际情况是: 兼容性的真正难点不在系统调用的存在性，而在每个系统调用的精确语义、每个错误条件的 errno 返回值、每个边界情况的处理方式。

`setresuid(-1, 1000, -1)` 的 NOCHG 语义, -1 表示该字段不修改。`prlimit64` 提高硬限制时返回 `Ok(0)` 但静默不生效。fsync 作用于目录 fd 返回 `EISDIR` 还是 `Ok(0)`。epoll_pwait sigsetsize=16 返回 `EINVAL` 还是接受。

用户态生态中的隐性契约同样关键: `/sys/class/drm/card0` 必须是软链，evdev minor 号必须从 64 起，`/run/udev/data/` 必须预填空文件。这些都不对应任何一个缺失的系统调用，但任何一个偏差都导致应用静默失败。

Linux 兼容性本质上是"成千上万个微小 ABI 的正确实现"，而非"约 400 个系统调用的存在性检查"。

### Slide 41: 从这次实验看 OS 教学

不写通用建议。只写这次实验让我意识到的、在课本和讲义里学不到的三件事。

**① 修内核与写内核是两种不同的技能。**

写内核是自上而下: 你决定设计，你定义接口，你控制复杂度。修内核是自下而上: 设计已经定了（Linux），接口已经定了（210 个 syscall + 数百个 ioctl + sysfs/procfs 格式），复杂度已经在了（15 年用户态生态的累积）。你没有控制的奢侈，你只能理解。

课程前三年教的全是"怎么写"。这次六周全在"怎么修"。修的技能与写的技能有本质区别。修需要读 man page 找边界条件、写最小复现用例、对照 Linux 确认预期行为、最小改动修复。这组方法在现有课程中未被作为独立技能教授。

**② 最难的 bug 源于用户态代码的假设从未被记录，而非内核代码本身写错。**

`/sys/class/drm/card0` 必须是软链。`evdev` minor 号必须从 64 起。`/run/udev/data/` 目录下必须有空文件。这三项分别花了数小时在"找"上，不在"修"上。找的过程: 应用崩溃 → 读应用日志 → 读中间库源码（libudev/libinput/libdrm）→ 发现某个判断条件 → 确认你的内核给了什么 → 发现偏差。

现有课程的调试训练是"我的代码哪里写错了"。这次实验的调试训练是"15 年前某人在 udev 里做了一个假设，从未被文档化，但今天你的内核必须满足它"。

**③ 真实应用是更好的测试框架。**

PostgreSQL 在 initdb 过程中测试了 fsync 对目录 fd 的行为、setresuid 的 NOCHG 语义、rename 后 page cache 保持、epoll CTL_MOD 事件保持。没有任何单元测试覆盖这些组合。问题不在于测试质量，在于测试编写者根本不知道这些组合存在。真实应用不知道你的内核实现细节，它只会用它认为理所当然的方式调用系统调用。这种"无知"使它成为最诚实的测试者。

课程的最终验收不必是对着 checklist 跑 N 个单元测试。使一个真实应用（PostgreSQL 或 Weston 级别的复杂度）在你的内核上完成一项具体任务, pass initdb 就够了，显示一帧就够了。但这个目标会迫使你面对所有你从未想到的边界条件。

---

## §7 最终产出（1 页）

### Slide 42: 最终产出汇总

```
实验1 (AI 流水线)
  ├── starry-harness 插件 (9 技能 + 3 代理 + 16 脚本)
  └── 支撑全部 32 个 PR 的开发、测试、审查全流程

实验2 (系统调用修复)
  ├── 11 个独立 PR
  ├── 覆盖: preadv/pwritev ABI, timerfd, mremap, sigaltstack,
  │     getrandom, copy_file_range, ftruncate, getgroups,
  │     loongarch64 smoke, epoll sigsetsize, user crash dump
  └── 深挖: timerfd (完整 fd 类型), preadv/pwritev (ABI 层), mremap (内存管理)

实验4A (PostgreSQL)
  ├── 13 个通过 PG 发现的 PR
  ├── 深挖: 凭证子系统, SA_RESTART, IPC 死锁, VFS rename, epoll LT
  └── PostgreSQL: postmaster 启动, initdb, 基本查询通路

实验4B (Wayland/Weston)
  ├── 8 个 Wayland 专属 PR
  ├── 深挖: DRM/KMS 双路径, sysfs 软链+minors, memfd F_SEAL_*,
  │     SCM_RIGHTS byte-mark, evdev EVIOCGPROP, netlink, aarch64
  └── Weston: drm-backend + libinput + wayland 协议渲染通路

架构分析
  ├── 5 个有利于 AI 开发的架构特征
  ├── 5 个产生摩擦的架构特征
  └── 6 个重新设计方向

最终产出: 32 个已合入 PR, 30+ 回归测试用例, 4 架构覆盖
```

---

### Slide 43: 致谢 / Q&A
