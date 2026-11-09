# GARAgent 重构计划（2026-11-09 冻结）

本文是重构的单一事实源。所有决策、命名、架构约定与分阶段执行顺序在此冻结；
执行中不再临时变更，变更需回到本文修订并注明日期。

## 0. 目标

1. 消除冗余（删四个遗留 executor，只留 AdaDeduce）。
2. 除 third_party 外，推理相关代码收敛为单一 `gar::core` 库；图/模式数据结构
   直接复用 MatrixGraph，删除自建 `TripleGraph`/`VertexAttrs`/`CSRTopology`/`bridge`。
3. 结果与论文 AdaDeduce（Algorithm 6）语义一致——尤其保留代码已落地的
   G∪Γ + delta generation + partial action 挂起语义（见
   `docs/fixpoint_implementation_plan.md` 的「通用 GAR 方案」）。

## 1. 冻结决策

### 1.1 分层与依赖 DAG

```
core/                          # gar::core（算法 + 语义）
├── rule/                      # GarRule = GARPatternArrays(MatrixGraph) + X/consequent/quality
├── hdc/                       # 纯数值内核（零依赖）
├── analysis/                  # PredicateTable + DependencyGraph（只依赖 rule）
└── engine/
    ├── chase/                 # ChaseEngine（--mode chase）
    └── agent/                 # AdaDeduceExecutor + Scorer/Policy/ValueModel/Witness（--mode agent）

tools/                         # 消费者（用 core，不进库）
├── cli/  cli_common/  miner/  scripts/

third_party/MatrixGraph/       # 数据结构 + 基础图计算
```

依赖只允许向下，无环：
`rule ← analysis`；`engine ← rule + analysis + hdc + MatrixGraph`；
`tools ← core`。`analysis` 只依赖 `rule`（依赖图 D_{Σ,g} 从规则集构造，不碰图 G）。

### 1.2 MatrixGraph 边界（关键）

- **MatrixGraph 只负责**：数据结构（`ImmutableCSR`/`Attribute`/`GARPatternArrays`）
  + 基础图计算——**子图匹配（同态，CPU+GPU）**、属性读取。
- **core 自己写（非基础计算）**：X 谓词求值（含 ML）、事实合并 / Γ 维护、
  增量匹配（delta）策略、依赖图分析、评分、策略、演绎控制流。

即：`Match(pattern, graph) -> matches` 是 MatrixGraph 的原语；core 在其上
实现「何时、对哪些子模式做增量重匹配」的推理逻辑。

### 1.3 数据结构

- 图 G = MatrixGraph `ImmutableCSR`（删 `graph::TripleGraph`）。**G 固定不变
  （immutable），派生事实不写回 G**；派生事实由 core 用可变事实集 Γ 维护
  （带版本号，供动态失败重准入），匹配在 G∪Γ 上进行——MatrixGraph 的
  `Match(pattern, G, Γ)` 把 Γ 作为增量边集合输入。
- 模式拓扑 + label = MatrixGraph `GARPatternArrays`（删 `CSRTopology` 与
  `bridge/pattern_bridge` 零拷贝转换）。
- 属性 = MatrixGraph `Attribute`（删 `VertexAttrs`）。
- X 谓词 / consequent p₀ / quality / pivot / 字典：MatrixGraph 无对应结构，
  留在 `core::rule`。
- **逆槽位**：方案 A——物化双向边，`r^-1` 的 label = 基础 label + num_relations，
  `IsInverse`/`BaseRelation` 语义不变（`r^-1(x,y) ≡ r(y,x)` 的归一化仍在 core，
  矩阵层只负责把两个方向的边都存好）。

### 1.4 模式与变体：用 flag，不用类名区分

| 论文基线 | CLI | 组成 |
|---|---|---|
| Chase | `--mode chase` | `ChaseEngine` |
| GoalChase | `--mode agent --policy deterministic --scorer null` | AdaDeduce + NullScorer |
| Deterministic | `--mode agent --policy deterministic` | AdaDeduce + DeterministicPolicy |
| Random | `--mode agent --policy random` | AdaDeduce + RandomizedPolicy |
| Learned | `--mode agent --policy learned` | AdaDeduce + LearnedQPolicy |

- `--mode chase|agent` 两个模式；`--policy`/`--scorer` 选策略/评分器变体
  （策略模式，接口 `Policy`/`Scorer` 下的实现）。
- 删四个遗留 executor：`GreedyExecutor`/`BranchBoundExecutor`/`BackwardExecutor`/
  `BackwardBnBExecutor`，以及 `motif_generator.cpp`（bnb 专用）、`AgentResult::fwd_stats`。
- 现 CLI 的 `--mode full` 改名 `--mode chase`。

### 1.5 命名（对齐论文术语 + Google C++ Style）

| 现状 | 改为 | 依据 |
|---|---|---|
| `FixpointExecutor` | `AdaDeduceExecutor` | 论文算法名，消除「fixpoint 作区分名」歧义 |
| `fixpoint_session` | `DeduceSession` | 一次 goal 演绎的会话状态 |
| `fixpoint_matcher` | `PartialMatcher` | 可挂起动态边的部分匹配 |
| `fixpoint_working_set` | `WorkingSet`（ReadySet + SuspendedSet） | R/W 工作集 |
| `fixpoint_preconditions` | `PreconditionCache` | 前提结果缓存 H |
| `fixpoint_producers` | `ProducerTracker` | producer 存活追踪 |
| `Motif` | `Action`（或 `GroundedAction`） | 论文 §3.1 术语「inference action μ=(φ,h)」 |
| `DeductionEngine` 目录 `engine/deduction` | `ChaseEngine` / `engine/chase` | 「deduction」是通称，chase 专指全闭包 |
| fixpoint loop（注释/正文） | revival loop | 第二个循环做的是复活 W，不是到不动点 |
| `GpuDeduceEngine` | `GpuChaseEngine` | 与 `CpuChaseEngine` 命名一致 |

Google style：类型/函数 PascalCase，变量 snake_case（类成员尾下划线 `_`），
常量 `k`+PascalCase，命名空间 `gar::core::…`（snake_case，目录镜像命名空间），
文件 `snake_case.cc/.h`，头文件 self-contained + `GAR_CORE_<PATH>_<FILE>_H_` 守卫。

### 1.6 设计原则（后续迭代都要遵守）

1. **注释**：文件头/类头说明职责与不负责什么；关键函数写「为什么」而非「做什么」；
   跨模块不变量（匹配口径、逆槽位归一化、静态/动态失败分类）必须注释。
2. **高内聚低耦合、输入输出清晰**：跨层无全局可变状态；会话状态局部在
   `DeduceSession`；依赖注入经 `ReasoningAgent`（composition root）。
3. **TBB/GPU 通用性**：算法层与后端解耦——匹配走 MatrixGraph 的
   CPU(TBB)/GPU 两后端同一接口，切换后端不改算法代码；core 自己的非基础
   计算用 TBB，后续可迁 GPU 的以同一接口隔离。
4. 参考 Google C++ Style。

## 2. 分阶段执行（每阶段可编译、可回归）

### Phase 0 — 基线 + 决策记录（本文）
- 提交/暂存现有未提交改动（`fixpoint_*.cpp` 等属用户在途工作，先冻结为基线）。
- 记录回归锚点：synth 的 fixpoint 路径、rules512 锚点 goal。

### Phase 1 — 消冗余（先删遗留，缩小后续面）
- 删 4 个遗留 executor + `motif_generator.cpp` + `fwd_stats` + CLI 映射
  （`--executor greedy|bnb|backward`）。
- 只保留 AdaDeduce 路径（`--mode agent`）。
- **验收**：build 通过；synth + rules512 锚点回归与 Phase 0 逐字节一致。

### Phase 2 — 换底 MatrixGraph（风险最高）
- 删 `TripleGraph`/`VertexAttrs`/`CSRTopology`/`bridge`；`core::rule` 改为
  `GARPatternArrays` + 语义字段。
- MatrixGraph 侧：新增同态子图匹配（CPU+GPU）；逆槽位物化双向。
- core 侧：X 求值、事实合并/Γ 维护、delta 匹配策略自己写，调用 MatrixGraph 的
  `Match` 原语。
- **验收**：miner/chase/agent 三方匹配口径一致；chase 闭包一致；agent 派生 ⊆ chase。

### Phase 3 — 并入单 core
- 建 `gar::core` 单一 target，include 前缀 `gar/core/...`，命名空间 `gar::core::…`。
- 目录镜像命名空间（保留子目录，不物理平铺）。
- `tools/` 拆 `cli/`/`cli_common/`/`miner/`/`scripts/`，apps 并入。

### Phase 4 — 语义核对
- 跑 synth 回归 + 关键锚点，确认 G∪Γ + delta + 复活语义未破坏。
- 对照论文：定理 4 可靠性（派生 ⊆ chase 闭包）、定理 5 完备性（公平执行下
  goal 可达）、定理 6 终止（(rule,embedding) 去重）。

### Phase 5 — 文档同步
- 重写 README / `docs/agent_design.md` / `docs/commands.md` 的目录与分层描述。

## 3. 风险与已知影响

- **纯 CPU 开关废弃**：core 依赖 MatrixGraph（CUDA），`-DGAR_ENABLE_MATRIXGRAPH=OFF`
  不再支持（「尽可能用 MatrixGraph」的必然结果）。
- **MatrixGraph 是 submodule**：改 MatrixGraph（加同态匹配）需提交到
  `hsiaoko/MatrixGraph` 并更新 submodule 指针。
- **实验失效**：现有实验（TripleGraph 输入）需在 Phase 4 后重跑锚点，不承诺
  全矩阵复现。
- **论文算法语义**：保留代码已落地的「通用 GAR 方案」，不倒退到「只在静态 G
  匹配一次」的旧伪代码（见 `docs/fixpoint_implementation_plan.md` §3）。
