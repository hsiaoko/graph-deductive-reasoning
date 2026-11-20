# AdaDeduce 补全与重构计划

状态：讨论稿。本计划只定义改造顺序和验收条件，不授权修改实现或启动长实验。

## 目标与完成判据

目标是让 `core/` 正确实现 `paper/paper.pdf` 的算法和实验变体，同时保留现有 proof-star、GPU 和 Jev 工作。

完成时必须满足以下条件。

1. Chase、DepOnly、Deterministic、Random、Learned 和 AdaDeduce 都有明确、可复现的配置。
2. 所有 agent 变体共享同一个精确演绎执行器。策略只改变顺序，不改变事实语义。
3. 论文中的 ordered top-k batch、goal-aware score、HDC evidence、cost-to-go 和冷启动 guide 都有对应实现。
4. 当前 proof-star 排序作为独立扩展保留，不与论文中的 action-level guide 混为一谈。
5. 单 goal 和 batch 入口使用同一份执行配置。CPU 是默认实验后端，GPU 只在显式选择时启用。
6. 所有派生通过同一规则集的 Chase 闭包对账。cap、timeout 和未命中不得隐藏。
7. Release 构建对 C++ 和 CUDA 使用 `-O3`。构建配置不把第三方测试或本机指令集选择泄漏给调用方。

## 当前实现判断

AdaDeduce 主执行器已经存在。`AdaDeduceExecutor` 和 `DeduceSession` 已实现 goal-cone、完整与部分匹配、ready set、suspended set、增量匹配、前提缓存和 revival loop。保留这套实现，不再写一个同名执行器。

当前缺口主要在论文语义、变体定义和实验口径。

| 优先级 | 已确认的问题 | 代码证据 | 处理 |
| --- | --- | --- | --- |
| P0 | 论文按 `d(p0)` 计算 relevance，代码按规则全部谓词距离之和并加入 confidence | `core/engine/agent/src/scorer.cpp` | 建立论文标准 scorer，旧公式只作为有名字的历史实验配置 |
| P0 | 论文 HDC 是 set-based normalized inner product，代码使用 cosine，并对重复 witness 重复累加 | `scorer.cpp`、`witness.cpp` | 修正 set 语义和估计公式，再验证 Proposition 3 的适用条件 |
| P0 | 论文选择 ordered top-k batch，当前 `Policy` 每次只选一个 action | `policy.h`、`deduce_session.cpp` | 增加唯一的 batch selection 接口，保留 batch size 1 作为现有行为对照 |
| P0 | 当前 revival 把已满足的 W action 重新放入 R，论文 Algorithm 7 则直接完成它 | `deduce_session.cpp` 的 `RevivalPass` | completion loop 直接派生，不再次匹配、静态求值或调用 policy |
| P0 | 论文伪代码在固定 G 上匹配，当前实现有意支持 G∪Γ、partial 和 delta discovery | `partial_matcher.*`、`deduce_session.cpp` | 保留现有通用语义并补齐论文描述，不倒退为固定 G |
| P0 | `total_cost` 只含单位 action 求值，未含论文要求的 generation 和 selection；`proof_cost` 累加所有成功派生，不是 goal proof DAG | `deduce_session.cpp` | 分离 logical action count、execution cost、wall 和 proof-DAG cost |
| P0 | 含 ML predicate 的规则被跳过，实际没有 ML oracle | `rule_support.h`、`predicate_evaluator.cpp` | 增加精确 ML 求值接口；未配置 oracle 时显式报告不支持，不能静默跳过 |
| P0 | 当前 LLM guide 对 star 给概率并排序，论文附录要求编译 feature-conditioned action priority rules | `policy.cpp`、`docs/jev_guide.md` | 两种机制分开命名和配置，分别做对照 |
| P1 | DepOnly 通过 `--mode chase --goal ... --goal-early-stop` 隐式组成，变体身份不清晰 | `tools/cli/gar_reason.cu` | 提供显式 DepOnly 配置，复用固定顺序、无 R/W 的 goal-cone Chase |
| P1 | 当前实验中的 AdaDeduce 是 deterministic action policy 加 proof-star，和论文中 learned/hybrid 的文字定义不一致 | `experiments/overall-performance/ogbl-biokg/run.py` | 将 proof-star 臂独立命名；冻结论文主臂和扩展臂的组成 |
| P1 | batch 入口没有复制单 goal 的全部 `ExecConfig` 字段，也没有完整记录 guide 开销 | `tools/cli/agent_batch.h` | 消除第二份配置装配逻辑 |
| P1 | 初始图已含 goal 时，`ReasoningAgent` 仍可能先因 goal 不在 DG 而抛错 | `reasoning_agent.h`、`deduce_session.cpp` | 在 DG 检查前返回已满足状态，并覆盖正向和反向入口 |
| P1 | `FailureRegistry` 在主执行器中未使用，`utility` 从未更新，`SelectRule` 没有实际调用方 | 对应 public headers | 迁移调用方后删除这些伪扩展点 |
| P1 | 仓库没有自己的 CTest 测试树，现有验证依赖 examples、CLI 和实验脚本 | 根目录和各模块 CMake | 先固化可重复的真实 CLI 验证；是否新增测试体系另行批准 |

当前 Release 构建已使用 `-O3 -DNDEBUG`。证据是 `build/core/engine/agent/CMakeFiles/gar_engine_agent.dir/flags.make`。CMake 工作聚焦配置边界，不重复添加全局优化 flag。

## 架构比较与选择

两个独立候选都建议以 batch 为策略单位、直接完成 W、显式管理 action identity，并分离论文 guide 与 star 排序。本计划采用候选一的状态所有权和 `ActionRegistry` 设计，并吸收候选二的完整成本分项与 backend 重建检查。

两个候选都建议把固定 G 设为唯一标准路径，并把 MatrixGraph 的 Chase 逻辑下沉为纯匹配。本计划拒绝这两点。现有 G∪Γ、partial 和 delta 是已确认的通用 GAR 语义，现有 Chase 行为也保持不变。需要修改的是论文和代码的术语对齐，不是删除这些能力。

候选中的根测试树和大量新 fixtures 不在本计划内。当前仓库没有自己的测试树，先使用已有 examples、真实 CLI、闭包对账和实验脚本验证。

当前工具为两个候选分配了相同模型，无法进行不同模型供应商的交叉评审。最终选择由主审查按语义归属、状态所有权、改动范围和验证可行性完成。

## 目标架构

保留现有模块和 `gar::core` 聚合 target。不要把所有源码合成一个巨型 target，也不要为每个算法步骤增加基类。

```text
core/
  rule/                 GAR、predicate、MatrixGraph pattern view
  graph/                base graph、Gamma、动态索引、精确谓词求值
  analysis/             PredicateTable、DependencyGraph、goal analysis
  hdc/                  无数值语义外依赖的 HDC kernel
  engine/
    chase/              full closure 和 DepOnly 使用的固定顺序 Chase
    agent/
      execution/        AdaDeduce 两循环、R/W、delta、revival
      policy/           action batch policy 和独立 star order policy
      scoring/          paper score、evidence、cost model
      guidance/         typed LLM guide compiler 和客户端适配
      learning/         cost-to-go model、proof-star model、trace structures

third_party/MatrixGraph  基础数组、匹配组件、CUDA buffer、stream 和 kernel
third_party/llm2jev      模型服务，不进入 C++ 推理语义
```

### 状态所有权

| 状态 | 唯一所有者 | 约束 |
| --- | --- | --- |
| 初始图和字典 | 共享只读 graph 输入 | 多 goal 不得修改它 |
| 当前 Gamma 和派生日志 | 每个 goal 的 `AgentState` | 后续可改成 base graph 加 per-goal delta overlay，避免整图复制 |
| H、witness 和 provenance | 每个 goal 的 execution history | 只读评分不能修改历史 |
| R、W 和 producer 状态 | `DeduceSession` | 不暴露给 CLI 或模型客户端 |
| action identity 和完整记录 | session 内 `ActionRegistry` | `(rule, embedding)` 是语义 key，session-local id 只是句柄；replay 持久化语义 key |
| CPU/GPU scratch、stream 和 resident buffers | `MatchBackend` | 逻辑执行器只提交 `MatchRequest`；backend 可从 FactStore 重建以对账增量缓存 |
| 模型和 LLM 客户端 | `ReasoningAgent::Components` | 原始指针只在一次 Run 的所有权范围内使用 |
| policy RNG 和在线选择状态 | 每次 Run 的 policy instance | 多 goal 不共享随机流或剩余预算 |

### 只保留真实变化点的接口

- `Policy::SelectBatch` 返回有序 action id 列表。Deterministic、Random、Learned 和 guide-enhanced policy 共用该入口。
- `StarOrderPolicy` 只负责 star 顺序。input、goal-distance、proof-efficiency 和 LLM-star 是独立实现。
- `Scorer` 负责论文的 relevance、evidence 和 residual cost。不要在 policy 中重新计算这些量。
- `CostModel` 负责前置条件成本。`ResidualCost` 必须调用它，不能对非边谓词硬编码单位成本。
- `MatchBackend` 保持 CPU/GPU 边界。复用 MatrixGraph 组件，不要求上游增加 `GARAgentMatch`。
- `MlPredicateEvaluator` 负责精确 ML predicate。模型实现可以外置，执行器只接收布尔结果和测量成本。
- `Executor` 不为假设中的未来算法保留。若 backward 只用于研究，将它放到明确的 experimental 入口。

CLI 只解析字符串并生成 typed config。`core` 中的 factory 负责组件装配。`AgentModeArgs` 不再同时承担文本解析、组件所有权和实验变体定义。

## 变体契约

| 名字 | 固定顺序执行器 | action 选择 | star 顺序 | 学习或模型输入 |
| --- | --- | --- | --- | --- |
| Chase | 全规则 Chase | N/A | 输入顺序 | 无 |
| DepOnly | goal-cone Chase，单线程参照 | N/A | 输入顺序 | 仅 dependency filter |
| Deterministic | AdaDeduce | 论文 score 的 deterministic top-k | 固定配置 | 无 |
| Random | AdaDeduce | 同一 score 的 softmax batch | 与 Deterministic 相同 | 仅随机种子 |
| Learned | AdaDeduce | learned cost-to-go top-k | 与 Deterministic 相同 | action-level Q model |
| AdaDeduce | AdaDeduce | learned/hybrid policy | 固定配置 | symbolic guard，可选冷启动 guide |
| AdaDeduceStar | AdaDeduce | 显式记录所选 action policy | proof-efficiency 或混合残差排序 | proof-star model，独立于论文主臂 |

`AdaDeduceStar` 是当前工作的保守命名。若后续论文把 star ordering 纳入主方法，再统一修改论文、CLI 和实验脚本，不能只在某个实验脚本中改变 AdaDeduce 的含义。

fairness、candidate cap、wall limit 和 budget 是运行条件，不作为隐藏变体。只有完整候选发现且公平执行时，才能引用论文的 goal-level completeness。

## 落地顺序和依赖

```text
PR0 baseline and contract
├── PR1 CMake boundary                         独立，可紧随 PR0
└── PR2 typed variants and shared config
    └── PR3 paper scoring and HDC
        └── PR4 cost ledger and proof provenance
            └── PR5 ordered batch policies
                ├── PR6 exact ML predicate boundary
                └── PR7 paper guide and star extension separation
                    └── PR8 module cleanup and state isolation
                        └── PR9 frozen CPU/GPU experiment validation
```

PR6 可在 PR5 后独立进行。其他阶段按依赖顺序执行。每个阶段必须保留当前未提交工作，不覆盖用户已有修改，也不把 MatrixGraph 子模块变化混入无关提交。

PR2、PR3、PR5 和 PR7 会改变配置、排序或外部协议，合并前需要你的审查。PR9 只生成实验结果，结论也需要单独审查。

## PR0：冻结语义和基线

- 依赖：无。
- 可改文件：本计划、实验设置文档、现有运行脚本中的配置清单。
- 改变：逐项确认上表变体，记录二进制、源码 diff、规则、goal、seed 和模型哈希。
- 结果：得到修改前 CPU 基线和现有 GPU 参照。旧 BioKG 中止队列的结果不能作为正式基线。
- Unit checkbox：构建并运行现有 `load_rule`、`rule_set_info`、`value_model_check`。
- Live checkbox：在 `data/synth` 上运行 Chase、DepOnly 和四种 agent policy，核对 goal 和派生闭包。
- Perf checkbox：先测量 FB15K-237 36 goal 的 wall、work counters 和运行间波动。后续行为保持型改动若中位 wall 回退超过 5%，且差值大于三倍观测波动，则不得进入下一阶段。

## PR1：收紧 CMake 配置

- 依赖：PR0。
- 可改文件：根目录、`core/`、`tools/`、`examples/` 的 CMake 文件。MatrixGraph 修改单独审查。
- 改变：使用标准 `CMAKE_CUDA_ARCHITECTURES`，沿用 CMake 标准 Release 配置提供的 `-O3`，隔离 MatrixGraph 的 `BUILD_TESTING` cache 副作用，并默认关闭本机指令集。
- 结果：同一路径可生成可移植 Release 和显式 native Release。CUDA 仍是 MatrixGraph 依赖，CPU 实验不需要启动 CUDA 计算。
- Unit checkbox：检查 clean configure、target 编译和实际 flags，不把已有 build cache 当作新配置证据。
- Live checkbox：运行 synth CPU agent、CPU Chase 和显式 GPU Chase，核对结果集合。
- Perf checkbox：使用 PR0 的短锚点。行为不变时采用 5% 加三倍波动门槛，并记录实际 CUDA architecture。

## PR2：统一变体和执行配置

- 依赖：PR0。
- 可改文件：`core/engine/agent/` 的 config 和 factory、`tools/cli/agent_mode.h`、`agent_batch.h`、`gar_analyze.cu`、`gar_reason.cu`。
- 改变：typed variant、scorer、policy、star order 和 backend 替代字符串散落判断。单 goal、batch 和 analyze 共用一个 config-to-executor 转换。
- 结果：同一配置在所有入口产生相同 action 顺序、派生和统计字段。DepOnly 有显式入口。
- Unit checkbox：运行现有 examples，并检查非法组合在 CLI 边界报错。
- Live checkbox：对同一 synth goal 比较单 goal 和 batch 的 chain、summary 和 termination。
- Perf checkbox：该 PR 只移动配置，采用 PR0 的 5% 行为保持门槛。

## PR3：对齐论文 scoring 和 HDC

- 依赖：PR2。
- 可改文件：`scorer.*`、`witness.*`、`motif.h`、`core/hdc/` 及对应实验配置。
- 改变：实现 `r(g,mu)=gamma^d(p0)`、set-based evidence、论文 normalized inner product 和真实 residual cost。空前置条件集合的 evidence 为 1。
- 结果：Exact 与 HDC 的输入集合一致。重复 witness 不改变 evidence。HDC 只影响排序。特征含义变化后提升模型 schema 版本，拒绝把旧权重用于新特征。
- Unit checkbox：用现有数值检查程序验证模型加载和纯数值路径；不新增未批准的测试框架。
- Live checkbox：在 synth 和冻结小 goal 上比较 exact、HDC、null 的派生与 Chase 闭包。
- Perf checkbox：分别报告 score wall、HDC dimension、witness 数和总 wall。不能把减少 action 数直接解释为端到端加速。

## PR4：修正成本与证明口径

- 依赖：PR3。
- 可改文件：`executor.h`、`deduce_session.*`、`state.h`、输出和 trace 聚合脚本。
- 改变：建立 cost ledger，区分 action count、generation、selection、evaluation、guide 和 wall。通过 provenance 提取 goal proof DAG 后再计算 proof cost。
- 结果：`proof_cost` 不再包含无关派生。Q 标签来自选定 action 后的真实剩余成本，失败 rollout 不获得伪有限标签。
- Unit checkbox：运行现有 value-model 和 trace 检查，核对 schema 版本。
- Live checkbox：重放一个含无关派生和动态挂起的 goal，检查 proof DAG、总搜索成本和 trace。
- Perf checkbox：ledger 和 provenance 的自身开销必须单列。行为不变锚点采用 5% 门槛。

## PR5：实现 ordered batch policies

- 依赖：PR4。
- 可改文件：`policy.*`、`working_set.*`、`deduce_session.*`、policy 训练脚本。
- 改变：用一个 `SelectBatch` 取代重叠的 score-view、full-dossier 和转发接口。实现 batch-start score、顺序求值、batch 内静态检查复用和 fair fallback。completion loop 直接完成已满足的 W，不把它重新交给 policy。
- 结果：k=1 可按语义 key 重放 PR4 的选择，k>1 符合论文 Deterministic、Random 和 Learned 定义。W 复活不再产生第二次静态求值。
- Unit checkbox：现有 policy/model 可执行检查必须通过，replay 对非法语义 key 必须报错。
- Live checkbox：同一 synth goal 上运行 k=1 和 k>1，比较派生、goal、静态检查复用和公平选择记录，并确认 W completion 不调用 match 或 policy。
- Perf checkbox：报告 batch size、selection wall、真实求值数和 wall。只有运行间波动小于效果时才接受加速结论。

## PR6：补全精确 ML predicate 边界

- 依赖：PR5。
- 可改文件：`core/graph/predicate_evaluator.*`、`rule_support.h`、agent factory、外部模型适配文档。
- 改变：注入精确 ML evaluator，记录调用和成本。未配置时对含 ML 规则给出显式不支持状态。
- 结果：含 ML 的 GAR 不再被静默跳过。CPU/GPU 后端不能把模型预测当作未经确认的图事实。
- Unit checkbox：使用现有 mock 路径验证布尔结果和错误传播，不调用外部模型。
- Live checkbox：运行一个含 ML predicate 的受控 GAR，分别检查 true、false 和 unavailable。
- Perf checkbox：记录 ML 调用数、批调用数和实际模型 wall。无 ML 的 PR0 锚点采用 5% 门槛。

## PR7：分离论文 guide 和 star 扩展

- 依赖：PR5。
- 可改文件：`core/engine/agent/guidance/`、policy、`deduce_session.cpp`、`tools/cli/`、`docs/jev_guide.md`。
- 改变：实现论文受限语法、类型检查、feature registry 和 action priority compiler。现有 LLM star 概率排序移到独立 star policy。
- 结果：每 goal 最多一次冷启动调用。无自由文本代码执行，无在线逐 action LLM 依赖。失败时回退到本地 policy。
- Unit checkbox：使用 mock 验证合法、非法、unknown feature 和空 guide。
- Live checkbox：对同一 goal 比较空 guide、action guide、LLM-star 和 proof-star，所有派生对账闭包。
- Perf checkbox：总 wall 必须包含请求、编译和特征求值。另报模型服务冷热状态，不能用热缓存结果代表新 goal。

## PR8：整理模块和状态边界

- 依赖：PR5、PR6、PR7。
- 可改文件：`core/` public headers 和内部文件、`tools/cli/`、README 与旧重构文档。
- 改变：删除已迁移的死接口，收窄 `Policy`，收敛 execution history，并让 batch 共享只读 base graph 和独立 per-goal delta。
- 结果：Gamma、H、R/W 和 backend 各有一个所有者。MatrixGraph 不接触 policy、Gamma 或 revival 语义。
- Unit checkbox：所有现有 examples 和验证程序通过。
- Live checkbox：单 goal、多 goal 和多 worker 运行得到与 PR7 相同的逻辑结果，worker 间无 Gamma 串扰。
- Perf checkbox：重点测量整图复制、索引重建、delta 和候选维护。内存下降而 wall 上升超过门槛时，保留测量并停止合并。

## PR9：冻结实验验证

- 依赖：PR8。
- 可改文件：实验配置、聚合和报告，不再修改算法。
- 改变：按冻结规则、goal、训练隔离和 seed 跑全部方法。默认 CPU，GPU 单独列表。
- 结果：FB15K-237 和 BioKG 都有逐 goal 原始记录、配对比较、分层统计、soundness 和资源指标。
- Unit checkbox：聚合脚本对缺失、timeout、cap 和错误行保持原状，不能静默删除。
- Live checkbox：每个方法至少完成一次 closure 对账。Random 保留全部 seed，确定性方法做计时重复。
- Perf checkbox：报告均值或中位数、离散程度、运行次数和主要限制器。若波动大于方法差异，结论标为 inconclusive。

## 明确不做的事项

1. 不重写已有 AdaDeduce 两循环执行器。
2. 不为“未来可能需要”创建通用基类、插件注册器或策略组合语言。
3. 不要求 MatrixGraph 新增 `GARAgentMatch`，不改变上游 Chase 行为。
4. 不把 GPU、LLM 或 proof-star 默认塞入所有 CPU 对照。
5. 不用编译通过代替真实推理验证，不用单次 wall 代替性能证据。
6. 不在本轮恢复旧的 BioKG 队列，也不修改论文 PDF 来掩盖代码差异。

## 讨论时需要确认的范围

本计划推荐把当前 proof-star 方法保留为 `AdaDeduceStar`，让 `AdaDeduce` 对应论文的 learned/hybrid 主方法。若你希望 AdaDeduce 始终表示“所有优化全开”，则应把 action-Q、proof-star、symbolic guard 和可选 LLM 全部写进论文主算法，并同步修改当前 PDF 中“star order 不变”的表述。

除此之外，计划不需要先增加抽象层。先修正语义和配置，再整理状态边界，最后才做性能优化。
