# 实验1: AI 开发流水线 (starry-harness)

## 1.1 动机

StarryOS 内核开发有三个痛点。

测试反馈周期长。内核修改后在 QEMU 中启动 StarryOS 并运行测试用例，单次迭代（build + rootfs + qemu + test）需要 3 到 5 分钟。在发现、修复、验证的循环里，大量时间耗在等待编译和启动上。

Linux 对照成本高。判断 StarryOS 的一个行为是否正确，需要在 Linux 上编译运行相同的测试程序，阅读 man page 确认预期行为，有时候还得阅读 Linux 内核源码确认实现细节。每一步都是手动操作。

重复性工作多。每个 bug 的修复流程高度相似（写测试、跑 Linux 对照、改内核、跑 StarryOS 验证、写报告），但随着修复数量增加，状态追踪和报告维护的负担线性增长。

`starry-harness` 把这三类重复工作自动化了。开发者只管理解语义、做出决策。

## 1.2 架构

```
starry-harness (Claude Code Plugin)
├── Skills (9)              ← 流水线步骤，由用户触发
│   ├── hunt-bugs           ← 发现, 测试, 对比, 修复, 报告 主循环
│   ├── test-app            ← Linux 应用兼容性测试
│   ├── benchmark           ← 性能基准测试
│   ├── audit-kernel        ← 内核内部审计 (锁顺序, 并发, 内存泄漏)
│   ├── review-quality      ← 代码质量门禁
│   ├── check-upstream      ← 上游 PR 去重检查
│   ├── start-submission    ← 准备 PR 提交
│   ├── evolve              ← 自主目标选择 + 持续开发循环
│   └── report              ← 结构化报告生成
├── Agents (3)              ← 专业分析代理
│   ├── linux-comparator    ← StarryOS vs Linux 行为对比
│   ├── kernel-reviewer     ← 内核代码审查 (正确性, 安全性, 并发)
│   └── bug-triager         ← Bug 分类, 严重性评估, 去重
└── Scripts (16)            ← 基础设施脚本
    ├── linux-ref-test.sh   ← 在 Docker Linux 上运行参照测试
    ├── man-lookup.sh       ← 查询 Linux man page 对应章节
    ├── strace-profiler.sh  ← 捕获应用的 syscall 序列
    ├── lock-order-graph.py ← 锁顺序图生成
    ├── pattern-scanner.py  ← 跨子系统模式扫描
    ├── journal-entry.sh    ← 日志条目追加
    ├── abi-check.py        ← ABI 兼容性检查
    ├── kernel-graph.py     ← 内核调用图生成
    └── ...
```

## 1.3 核心工作流: hunt-bugs

`hunt-bugs` 技能封装了从"怀疑某个 syscall 有问题"到"提交一个完整 PR"的过程:

```
Phase 1: 发现
  ├── 从 known.json 选择目标 syscall
  ├── 查阅 man page 获取预期行为
  └── 识别关键语义点和边界条件

Phase 2: 测试
  ├── 编写 C 测试程序 (交叉编译到 riscv64/aarch64/x86_64)
  ├── 创建 QEMU 配置 (qemu-{arch}.toml)
  ├── 在 Docker Linux 上运行参照测试
  └── 记录 Linux 输出作为参照基准

Phase 3: 对比 (linux-comparator agent)
  ├── 在 StarryOS QEMU 上运行相同测试
  ├── 逐字段对比输出差异
  └── 精确定位偏离 Linux 行为的点

Phase 4: 修复
  ├── 阅读 StarryOS 源码定位根因
  ├── 对照 Linux 内核实现确认修复方向
  ├── 实施修复
  └── 验证: 测试 FAIL → 修复 → 测试 PASS

Phase 5: 报告 (report skill)
  ├── 生成 bug 报告
  ├── 更新 known.json 状态
  ├── 追加 journal 条目
  └── 准备 PR 描述
```

## 1.4 三道工程护栏

**结构化输出强制。** 所有 AI 代理通过 JSON Schema 返回可验证的结果。`linux-comparator` 的输出格式: syscall 名、测试用例名、Linux 行为（返回值、事件数、数据匹配状态）、Starry 行为、判定结果（DIVERGENT 还是 MATCH）、根因提示。这个约束使流水线中的每个步骤输出可以被后续步骤无歧义消费，多轮迭代中的状态变化可追踪。

**状态记忆。** `os/StarryOS/tests/known.json` 是所有已发现 bug 的单一事实来源。每个条目包含 id、syscall、title、category、severity、status、fix_pr、test_case。这个状态机确保不会重复修复同一个 bug，不会在上游已修复后重复提交，可以随时生成进度统计和分类报告。

**审查者否决权。** `review-quality` 和 `kernel-reviewer` 代理独立于修复代理运行。它们的使命是找问题，不是批准。审查维度: 修复后的行为是否精确匹配 Linux？是否有 TOCTOU 窗口、UAF 风险、未初始化的内存？锁获取顺序是否一致？是否有死锁可能？错误路径上是否正确回滚了已分配的资源？回归测试是否覆盖了 bug 的所有触发条件？审查失败意味着修复必须重新修改，直到所有维度通过。

## 1.5 流水线数据

6 周内流水线支撑了 32 个 PR 的合入。约 8 个初始修复被审查拒绝回退重改，平均每 PR 迭代 1.5 到 2 次。跨架构测试覆盖 4 个架构。known.json 注册条目 50+，生成 bug 报告 20+。

## 1.6 与现有工具的对比

传统内核测试框架（LTP/kselftest）的测试是手动编写的，Linux 对照需要手动搭建环境，根因分析靠开发者自己看源码，报告手动撰写，状态追踪靠 Git issues。starry-harness 把测试生成自动化了（AI 根据 man page 生成），Linux 对照自动化了（Docker Linux + 自动对比），报告自动化了（结构化输出），代码审查自动化了一层（AI 预审查 + 人工终审）。

但架构决策仍需人工。AI 不能判断"应该用引用计数还是复制"这类需要全局权衡的选择。传递性影响分析也不足，修改锁顺序时 AI 能检查直接冲突但不能预判三阶传递性死锁。流水线还依赖开发者提出正确的问题，如果开发者没意识到某个边界条件需要测试，流水线不会自动发现。
