# 架构设计（最终版）

GARAgent 的最终形态：论文《Agentized Graph Reasoning via Policy-Guided
Deduction》的参考实现。本文是单一事实源；数字均来自本仓回归。

## 五级层化依赖

```
L4  apps/        kg_loader / match_pattern / mine_predicates / deduce /
                 deduce_gpu / validate_rules / gar_reason / gar_analyze
                 （+ examples/）
                  │
L3  engine/      deduction/（DeductionEngine 基类 + CpuChaseEngine +
                 GpuDeduceEngine）   agent/（ReasoningAgent + Executor
                 （greedy=增量 forward：policy 前置规则选择 + 持久候选
                 cache + delta 匹配 / bnb / backward / fixpoint=挂起-复活
                 两循环：R 由 action 级 policy 调度、W 由 fixpoint 复活，
                 见 docs/fixpoint_implementation_plan.md）+ Scorer/Policy 插件
                 + ValueModel（HDC 价值模型，bnb 可选排序，见
                 docs/hdc_value_model.md））
        miner/（ChainMiner / PatternMiner）
        bridge/（MatrixGraph 适配：LoadTripleGraph / LoadVertexAttrs /
                 ToPatternArrays）
                  │
L2  analysis/    PredicateTable / DependencyGraph（BuildDG + d(·) + SCC +
                 DerivationDepths + dot/yaml）/ RuleSetStats / GoalSampler
                  │
L1  graph/       TripleGraph / PatternMatcher / VertexAttrs / DerivedEdge /
                 EvalAntecedent / MergeRoundFacts+MarkAbsentEdges
    hdc/         Hypervector / HdcEncoder / EvidenceVector（bit-packed HDC）
                  │
L0  core/        规则表示与 IO：types / CSRTopology / Predicate / GARule /
                 RuleLoader / RuleStore
```

依赖只允许向下：L4 → L3 → L2 → L1 → L0。同级模块互不相识
（engine/deduction 与 engine/agent 是兄弟；miner 只依赖 graph+core；
bridge 依赖 graph+core+MatrixGraph）。

每模块一句话职责：

| 模块 | 职责 |
|---|---|
| core | GARule/Predicate/CSRTopology 的表示、YAML 加载与序列化（纯 CPU，仅 yaml-cpp） |
| graph | 图与语义内核：三元组存储与索引、同态模式枚举、X 谓词求值、轮末事实合并 |
| hdc | HDC 数值内核：bit-packed 超向量的 bind/bundle/permute/cosine 与证据向量（零仓库内依赖） |
| analysis | L2 分析服务：谓词身份与依赖图（d(·)、SCC、导出深度、序列化）、规则统计、目标采样 |
| miner | 规则挖掘（链式 + 通用拓扑），只读 graph |
| engine/deduction | 全闭包 chase 引擎家族（CPU oneTBB / GPU GARDeduce 双实现同一契约） |
| engine/agent | 策略制导演绎：motif 生成、见证、失败注册、打分、策略、执行器、ReasoningAgent |
| bridge | 唯一的 MatrixGraph 适配库（加载与布局转换） |
| apps | 组合与 IO；CLI 入口（gar_reason / gar_analyze）与旧 app 薄壳 |

## 依赖禁令（构建期纪律）

- **agent ✗ deduction**：兄弟引擎互不见面；--verify-against-closure 的
  组合在 apps 层（gar_reason/gar_analyze 同时链接两者）。
- **hdc 零依赖**：不 include core/graph 任何头；Fact→SymbolId 绑定在
  engine/agent/witness.h（hdc 只见裸 uint64）。
- **analysis 只依赖 core+graph**（禁止 engine/miner/hdc/MatrixGraph）。
- **MatrixGraph 只许两处 include**：bridge/ 与
  engine/deduction/src/gpu_deduce_engine.cpp（sanctioned 接触点；后者是纯
  .cpp——gar_deduce.cuh 只声明 __host__ API，kernel 在 matrixgraph_core
  内，实证 g++ 可编译）。

## 两个引擎家族的组合关系

```
                    apps（gar_reason / gar_analyze）
                   ┌──────────────────────────────┐
                   │  full 模式：DeductionEngine& │   agent 模式：ReasoningAgent
                   │   ├─ CpuChaseEngine          │   （composition root：
                   │   └─ GpuDeduceEngine         │     Generator/Scorer/Policy/
                   │   （同一 ChaseResult 契约）   │     Executor/CostModel 注入）
                   └───────┬──────────────┬───────┘
                           │              │
                 engine/deduction   engine/agent
                           │              │
        ───────────────────┴──────────────┴───────────────
        共享底座：graph（匹配/求值/合并）+ analysis（d(·) 打分依据）
        + hdc（证据打分 / bnb 价值模型特征）+ core（规则）
        对账：verify = agent 导出 ⊆ deduction 闭包（app 层组合）
```

两个引擎的唯一语义源头是 graph/ 的同口径原语：PatternMatcher（homomorphism
枚举 + label 后过滤）、EvalAntecedent（X 逐 embedding）、MergeRoundFacts
（规则序去重合并）。agent 的"应用一步"就是 chase 单步（EvalAntecedent +
结论不在 Γ），所以 soundness（Theorem 2）由构造成立。

## 关键语义口径（跨模块不变量）

- **匹配口径**：homomorphism（可重映射），顶点 label 后过滤，边 label 含逆
  槽位（id ≥ num_relations 为逆）——miner / chase / agent 三方逐行同构。
- **结论 grounding**：slot 取自 consequent.label；端点 pivot.size()==2 时
  pivot 覆盖 consequent(src,dst)（agent 的 ResolveRuleHead 与
  cpu_chase_engine.cpp 同构，注释互引）。
- **witness 语义**：应用时发生；成功 = 全部前提 + 导出事实，失败 = 匹配到的
  pattern 事实 + 已求值前提前缀（"a match computed is information gained"）。
  exact 集与 HDC 证据向量双写同步。
- **失败注册**：static（attr/ml/不可导出关系的边——只读 G）永久退休；
  dynamic（可导出谓词）按 Γ 版本重准入（本实现对"任何增长"重准入，是论文
  "失败谓词增长才重准入"的保守近似：完备性不受影响，可能多重试）。
- **price 剪枝**：price(μ) = w₀(φ) + d(p₀)；bnb 的 line-4 过滤
  （c+price≤B 且 <Ĉ）与 best-first 优先级（c + min price，延迟求值）。
- **certified 语义覆盖范围**：certified_optimal / certified_no_chain 仅在
  top_k=0（line 6 不截断）时成立；--max-candidates-per-rule 的 motif 生成
  截断是工程参数，证书覆盖"所见候选空间内的最优链"。
- **互逆槽位归一化**：r^-1(x,y) ≡ r(y,x)——依赖图谓词身份、agent 的
  Fact grounding、见证键三处一致（否则 d(·) 与见证集大面积失真）。
