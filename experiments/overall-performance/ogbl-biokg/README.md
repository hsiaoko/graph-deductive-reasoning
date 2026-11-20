# OGBL-BioKG overall-performance 试验设计

状态：**DESIGN ONLY，待检查；不得据此启动试验或填入结果。**

日期：2026-11-17。

## 1. 目的与范围

在同一 BioKG 图、同一冻结规则集和同一 36 个固定实例 goal 上，比较：

1. **Deterministic**：exact handcrafted score 的确定性贪心 action 选择。
2. **DepOnly**：只从 dependency tree 中选择 goal-relevant 规则并执行推理；其余优化全部关闭。
3. **Random**：使用 exact score 的 softmax 随机 action 选择。
4. **Learned**：现有 action-level numeric-Q，预测剩余求值成本。
5. **AdaDeduceStar**：proof-star 扩展方法，包括 proof-DAG 学习到的 proof-efficiency star 排序、exact deterministic action 选择和全部执行器优化；学习只允许在相同 goal-distance 桶内重排，不能跨桶提升高产远目标 star。

本试验不使用 LLM/Jev。Chase 只用于产生 ground truth 和提供非等价工作量参照，不列入五方法排名。

命名边界：AdaDeduce 执行器是多个 agent 臂共享的实现；本表中的 **AdaDeduceStar** 指 proof-star 扩展臂。**Learned** 专指 action-level numeric-Q，二者不混用。

## 2. 固定输入

- 图：`/ssd_data/zhuxk/graphs/KG/ogbl-biokg/immutable_csr`。
- 规模：93,773 个实体、51 种关系、4,762,677 条去重基础边。
- 顶点类型：drug、protein、disease、sideeffect、function 五类。
- 原始规则：`/ssd_data/zhuxk/graphs/KG/ogbl-biokg/rules`。
- 原始规则共 56,601 条，均为类型化规则，挖掘参数为 min-support=200、min-confidence=0.3。
- 所有方法共享相同的精确规则语义、顶点类型检查、前提求值和 goal 命中语义，但不共享优化能力：DepOnly 刻意关闭除 dependency-tree 过滤外的全部优化。

不得混用 FB15K-237 的规则、goal、模型或 ground truth。

## 3. 规则数量与冻结选择算法

### 主规则预算

主试验使用**最多 6,528 条规则**：

```text
51 个基础关系 × 每个 consequent 关系最多 128 条 = 6,528
```

这是预算上限，不要求凑满；规则不足的关系按实际数量保留，不把剩余配额转移给高频关系。

初始 32 条/关系的预实验冻结了 720 条规则，完整 Chase 正常到达 fixpoint，但 deep 闭包只有两个 consequent 关系，无法同时满足 deep 训练与 relation-disjoint 测试。经检查后决定将配额扩大到 128 条/关系，并在独立工作目录重建规则和 GT；不降低 goal 隔离要求。

不直接使用全部 56,601 条，原因是本试验比较 policy，而不是评估规则挖掘覆盖率；全量规则会让完整闭包和 goal-cone 搜索被大量相似类型化规则主导。也不照搬 FB15K-237 的 640 条，因为 BioKG 的关系数和类型化规则结构不同。

### 确定性选择

1. 先剔除执行器不支持的规则，记录剔除数量与原因；不得为增加数量而放松语义。
2. 按规范化 consequent 基础关系分组。
3. 组内按 body 长度、顶点类型签名、依赖图递归性和依赖深度分桶，并在桶间确定性交错，避免 quota 被浅层或单一结构占满。
4. 同桶只按稳定哈希排序；不再按 confidence/support 优先，避免把高置信度浅层规则集中选入。
5. 对每个 consequent 组最多取 128 条；不使用任何 goal 或方法运行结果来选择规则。
6. 冻结 `rule_ids.txt`、规则目录和 SHA-256；后续 GT、训练、测试只能读取这一冻结集合。

规则选择盐：`20261117-biokg-overall-rules-v2`。

在运行前记录实际规则总数、各 consequent 的规则数、body 长度分布和类型签名数。若实际规则数或结构与预期严重不符，应先修订设计，而不是静默调整。

## 4. Ground truth 与 goal 分层

### Ground truth

在冻结的同一规则集上运行完整 Chase 至 fixpoint，得到：

- 完整派生闭包；
- 每条事实首次派生的 Chase round；
- 规则、图和闭包的 SHA-256。

若完整 Chase 在资源包络内不能到达 fixpoint，则状态为 **GT unavailable**，不得把部分闭包当作完整 GT，也不得据此宣称 goal 完备性或 soundness 已对账。

Chase 时间与逐 goal agent 时间分开报告。Chase 计算完整闭包，agent 在单个 goal 派生后停止，二者输出工作量不同。

### 难度分层

按事实在完整闭包中的首次派生轮次分层：

- `round1`：第 1 轮；
- `round2`：第 2 轮；
- `deep`：第 3 轮或更晚。

该轮数是派生层次代理，不等于最短证明的严格边数。只能使用初始图中不存在、但在完整闭包中存在的固定实例正例作为 goal。

## 5. 36 个测试 goal 如何确定

测试集共 **36 个 goal**，在运行任何五方法测试前一次性冻结。每个 stratum 各 12 个，并同时保留两种隔离口径：

| 测试 cohort | round1 | round2 | deep | 合计 |
|---|---:|---:|---:|---:|
| instance-disjoint | 8 | 8 | 8 | 24 |
| relation-disjoint | 4 | 4 | 4 | 12 |
| 合计 | 12 | 12 | 12 | 36 |

### instance-disjoint

- 训练集不包含相同 goal spec；允许与训练 goal 共享 consequent 关系。
- 优先保持谓词多样性，每个 consequent 关系最多选择 2 个测试 goal。
- 用于检验对新实体实例的泛化。

### relation-disjoint

- 先按稳定哈希选择一小组 held-out consequent 关系，再从中采样 goal。
- 这些关系的**所有训练 goal 和训练轨迹都必须排除**。
- 用于检验模型是否只记住关系族。

候选按稳定哈希排序，不依据任何方法的命中、wall、派生数或胜负筛选。不得删除困难 goal、失败 goal 或模型表现差的 goal。

测试盐：`20261117-biokg-overall-test-v1`；关系分组盐：`20261117-biokg-overall-relation-split-v1`。

若某一层无法同时满足数量和关系隔离约束，则在运行前报告约束不可满足并修订设计；不得在试验中静默放宽。

## 6. 训练 goal 与模型隔离

另选 **24 个训练 goal**，每个 stratum 各 8 个：

- 与全部 36 个测试 spec 不重叠；
- 不包含 relation-disjoint cohort 的 held-out 关系；
- 不与测试 goal 共用 chain、proof、Q trace 或派生日志；
- 使用独立盐 `20261117-biokg-overall-train-v1` 冻结。

### Learned / numeric-Q

- 每个训练 goal 分别运行 Deterministic 和 Random(seed=7) continuation。
- 标签为从当前已选 action（含）到 goal 的剩余实际求值次数。
- 每条成功轨迹最多等距采样 256 行；未命中轨迹不赋予人工有限标签。
- 数值特征标准化和模型拟合只读取训练 goal。
- 不把未观察的 alternative action 伪造成有标签样本。

### AdaDeduceStar / proof-star

- 从训练 goal 的成功链反向提取 proof DAG，并按规则统计 proof hits 与轨迹总派生量。
- 效率标签为 `(proof_hits + 1e-6) / (1 + total_facts)`；proof 外规则只表示“不在本次提取的证明中”，不解释为逻辑上无用。
- 运行时分数为 proof-membership 减去预测 log-work，包含五个 support/body/距离交互项；所有特征和拟合只读取训练轨迹。
- `work_weight=0.1` 由训练 goal 的留出折中确定并冻结，不使用 BioKG 测试 goal 选择；`proof_star_weight=0.5` 只负责启用该排序。
- star 先按 consequent-to-goal DG distance 分桶：`d=0` 的直接 goal stars 按 proof efficiency 降序；`d>=1` 的前序 stars 锚定 baseline 首 star，仅对其余 star 做 baseline+efficiency 残差排序，禁止跨桶提升。
- 初始 `proof_star_min_cost=0`，不加入上一轮尚待验证的成本门控。

两个模型的 `trained_on`、特征版本、训练行数、随机种子和模型哈希必须随结果保存。

## 7. 五个方法臂的精确定义

| 方法 | 推理与选择配置 |
|---|---|
| DepOnly | 从 dependency tree 得到 goal-relevant 规则，按固定输入顺序执行 naive 完整模式匹配与派生；不使用 scoring、learning、优先级堆、增量 delta 匹配、前提缓存或 R/W 优化 |
| Deterministic | `--policy deterministic --scorer exact --star-order goal`，使用优化执行器，但不使用学习模型 |
| Random | `--policy random --scorer exact --star-order goal`，使用优化执行器和 score-based softmax |
| Learned | `--policy learned --scorer exact --value-model <biokg-q-numeric> --star-order goal`，使用优化执行器和 action-level numeric-Q |
| AdaDeduceStar | `--policy deterministic --scorer exact --star-order goal --proof-star-model <biokg-proof-efficiency-model> --proof-star-weight 0.5 --proof-star-min-cost 0`，优化全开：dependency 过滤、exact scoring、goal-head efficiency 排序与前序 baseline 锚定残差排序、增量匹配、优先级堆、前提复用、挂起/复活及其他执行器优化 |

DepOnly 只保留 goal 派生后的停止条件作为共同任务终止语义；candidate cap、wall 和内存限制只是资源保护，不算推理优化。`--scorer null` 仍运行在优化后的 AdaDeduce 执行器上，**不能冒充 DepOnly**。正式入口为 `--mode dep-only`，内部使用固定输入顺序、单线程的 goal-cone Chase。

DepOnly 使用独立 naive CPU 路径；其余四臂使用修复后的 CPU AdaDeduce backend。除非另行声明，所有臂均不启用 GPU 或 LLM。

共同参数：

- budget=1,000,000,000；
- candidate cap=100,000；
- fair interval=0；
- print=0；
- 内部 wall limit=900s，外部进程上限=1000s；
- 串行计时，进程树和整机已用内存上限均为 256 GiB；
- 只有整机已用内存低于 210 GiB 才启动下一个 run，不终止其他用户进程。

## 8. 运行规模与聚合

- 五方法 × 36 goals × seeds `{7,13,42}`，共 **540 个 goal-directed run**。
- Deterministic、DepOnly、Learned、AdaDeduceStar 不消费 action-policy 随机数，其三次运行是相同逻辑的计时重复。
- Random 的三个 seed 产生不同采样轨迹，必须逐 seed 保留，不能只报最优值。
- 每 goal、每方法先对三个 seed 的指标取中位数，再做 36-goal 等权宏平均。
- 另外分别按 round1/round2/deep 和 instance-disjoint/relation-disjoint 聚合，避免总体平均掩盖泛化差异。

主指标：

- 外部 wall 与内部 wall；
- 真实 action 求值数、派生事实数；
- 选择分支、ready 插入、完整匹配数；
- goal hit、soundness、timeout、candidate cap、OOM、RSS；
- Learned/AdaDeduceStar 的模型推理时间与训练规模。

选择分支包含零成本跳过已知结论，不能与真实求值混用。DepOnly 没有 ready heap、挂起/复活等优化执行器指标，对应字段标为 N/A，不填 0。cap、timeout、未命中和 OOM 原样保留，不因某方法失败而删除 goal 或调整限制重跑。

## 9. 正确性与报告判据

- 所有派生必须与同一冻结规则集的完整 Chase 闭包对账。
- 任何方法不得通过减少逻辑事实、绕过类型约束或提前伪造 goal 来换取速度。
- 主比较是同一 goal 上的配对 wall 和派生/求值变化，而不是只比较宏平均。
- AdaDeduceStar 的核心问题是：相对 Deterministic 和 Learned，能否减少 deep goal 的实际派生工作并转化为端到端 wall 收益。
- relation-disjoint 与 instance-disjoint 分开下结论；前者失败不能由后者成功替代。
- 所有五方法均报告完整分布、逐 goal 表和实际运行总时间，不用单一平均数替代完整结果。

## 10. 预期产物（运行后才会生成）

```text
.work_hdcv/overall-performance/ogbl-biokg/
  rules/                 # 冻结规则及哈希
  ground_truth/          # 完整 Chase 闭包
  goals.tsv              # 36 个冻结测试 goal 及 cohort/stratum
  train_goals.tsv        # 24 个冻结训练 goal
  models/                # numeric-Q 与 proof-star 模型及训练清单
  runs/                  # 540 个原始 run
  rows.tsv
  paired.tsv
  summary.json
  summary.md
```

本目录当前只保存试验设计，不包含任何 BioKG 性能结果。
