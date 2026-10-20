# GARAgent

面向学术论文的 C++ 图关联规则（Graph Association Rule, GAR）算法库。

- 图引擎：[MatrixGraph](https://github.com/hsiaoko/MatrixGraph)（vendored at `third_party/MatrixGraph`，CUDA）
- 规则表示：**CSR（RAPIDS/cuGraph 布局）+ YAML**，标量位宽与 MatrixGraph 对齐（VertexID/EdgeIndex/LabelID 均为 uint32）
  - CSR 描述规则模式图的拓扑依赖关系（外部 `csr_*.bin` 或 YAML 内联 `pattern.edges`）
  - YAML 描述节点 label、五种谓词、pivot、support/confidence 等
- 规则挖掘：AMIE 式规则挖掘器（`gar::miner`），从三元组图产出统一 YAML 规则集——
  链式规则（ChainMiner）与任意连通小拓扑规则（PatternMiner：星型、三角、带环等）两档

## 目录结构

```
├── CMakeLists.txt            # 顶层构建
├── core/                     # 规则表示与 IO（纯 CPU）
│   ├── include/gar/
│   │   ├── types.h           #   VertexID/EdgeIndex/LabelID（对齐 MatrixGraph：均 uint32）
│   │   ├── csr.h             #   CSRTopology：offsets/indices/edge_labels
│   │   ├── predicate.h       #   五种谓词：const/var/edge/exists/ml
│   │   ├── rule.h            #   GARule = (Q, X -> p₀)：pivot、可选 consequent、quality
│   │   ├── rule_loader.h     #   YAML/目录/规则集加载（容错解析）
│   │   └── rule_store.h      #   规则集 YAML 写出（挖掘结果序列化）
│   └── src/
├── graph/                    # 图与语义内核（纯 CPU，无 MatrixGraph 依赖）
│   ├── include/gar/graph/
│   │   ├── triple_graph.h    #   三元组存储（uint32 字典编码 + 逆关系 + 多种索引）
│   │   ├── pattern_matcher.h #   任意连通小拓扑的 embedding 枚举（CPU 回溯）
│   │   ├── vertex_attrs.h    #   顶点属性列（AttrColumn/VertexAttrs，纯 CPU 类型）
│   │   ├── fact.h            #   DerivedEdge：一轮 chase 导出的新事实
│   │   ├── predicate_evaluator.h  # EvalAntecedent：X 谓词对单个 embedding 求值
│   │   └── fact_merger.h     #   MergeRoundFacts/MarkAbsentEdges：轮末事实合并
│   └── src/
├── bridge/                   # MatrixGraph 适配层（唯一可 include MatrixGraph 的库）
│   ├── include/gar/bridge/
│   │   ├── graph_loader.h    #   ImmutableCSR → TripleGraph
│   │   ├── vertex_attrs_loader.h  # vertex_attr → CPU 属性列（.cu 内联头）
│   │   └── pattern_bridge.h  #   零拷贝桥接到 MatrixGraph GARPatternArrays
│   └── src/
├── hdc/                      # HDC 数值库（bit-packed 超向量，纯 CPU，零仓库内依赖）
│   ├── include/gar/hdc/
│   │   ├── const.h           #   维度档位 + ChooseDim（App I：(m+N)² ≤ D）
│   │   ├── hypervector.h     #   Hypervector：Bind(XOR)/Permute(循环移位)/Hamming/Cosine
│   │   ├── encoder.h         #   HdcEncoder：符号在线哈希 + v_p ⊕ ⨁ᵢ ρⁱ(v_aᵢ)
│   │   └── evidence.h        #   EvidenceVector：int16 坐标累加器 + cosine 读数
│   └── src/
├── analysis/                 # L2 分析服务（只依赖 core + graph）
│   ├── include/gar/analysis/
│   │   ├── predicate_table.h #   PredicateTable：谓词名 → 稠密 id（身份规则唯一来源）
│   │   ├── dependency_graph.h#   DependencyGraph：BuildDG + 反向 BFS/Dijkstra d(·) + Tarjan SCC + dot/yaml
│   │   ├── rule_stats.h      #   RuleSetStats：规则集与依赖形态统计
│   │   └── goal_sampler.h    #   GoalSampler：可导出槽位上的目标事实采样（纯采样，无 chase）
│   └── src/
├── miner/                    # GAR 挖掘
│   ├── include/gar/miner/
│   │   ├── chain_miner.h     #   链式规则挖掘（AMIE 式，support/confidence）
│   │   └── pattern_miner.h   #   通用拓扑规则挖掘（星型/三角/带环，逐层扩展）
│   ├── mine_rules.cpp        #   链式挖掘命令行
│   └── mine_graph_rules.cpp  #   通用拓扑挖掘命令行
├── engine/                   # 演绎引擎家族
│   ├── deduction/            #   全闭包演绎（chase）：抽象基类 + CPU/GPU 双实现
│   │   ├── include/gar/engine/deduction/
│   │   │   ├── deduction_engine.h    # DeductionEngine 基类 + ChaseConfig/ChaseResult 契约
│   │   │   ├── cpu_chase_engine.h    # CpuChaseEngine：拓扑匹配 + X 谓词 + 迭代至不动点（oneTBB）
│   │   │   └── gpu_deduce_engine.h   # GpuDeduceEngine：匹配/X/head 发射在 device（GARDeduce）
│   │   └── src/
│   └── agent/                #   策略制导演绎（policy-guided deduction，论文核心）
│       ├── include/gar/engine/agent/
│       │   ├── reasoning_agent.h     # ReasoningAgent：composition root
│       │   ├── executor.h            # Executor 基类 + GreedyExecutor + BranchBoundExecutor（HybNS 分支定界）
│       │   ├── motif_generator.h     # NaiveMotifGenerator（d(p₀)<∞ 硬过滤前置）
│       │   ├── scorer.h              # ExactScorer / HdcScorer（rel_fam/rel_evd/dcost/price）
│       │   ├── policy.h              # Deterministic / Randomized / HybridNeuroSymbolic
│       │   ├── state.h               # AgentState（Γ 只增 + version 计数）
│       │   ├── failure_registry.h    # static 永久退休 / dynamic 按版本重准入
│       │   ├── witness.h             # WitnessLog：exact 集 + HDC 证据向量双写
│       │   ├── cost_model.h          # CostModel/UnitCostModel（w₀ 下界）
│       │   ├── fact.h / goal.h / motif.h
│       │   └── ...
│       └── src/
├── apps/
│   ├── kg_loader.cu          # KG 装载：TripleGraph -> MatrixGraph ImmutableCSR + CLI 展示
│   ├── match_pattern.cu      # 规则模式匹配：GARMatch（GPU）物化匹配表 + 属性谓词统计
│   ├── mine_predicates.cu    # 阶段三：在规则匹配上挖 const/var 属性谓词（回填 X）
│   ├── deduce.cu             # 演绎：规则集在 KG 上的完整逻辑闭包（CPU chase，薄壳）
│   ├── deduce_gpu.cu         # 演绎 GPU 版（薄壳）：匹配 + X 求值 + head 发射在 device（GARDeduce）
│   ├── gar_reason.cu         # 演绎统一入口：--mode full（cpu|gpu）/ --mode agent（策略制导，--goal/--budget/--policy/--scorer）
│   ├── validate_rules.cu     # 规则检验：重算 pivot-pair support/confidence 并与挖掘值对账
│   ├── full_deduce_cli.h     # gar_reason/deduce/deduce_gpu 共享：CLI 解析 + 加载 + 输出
│   └── closure_output.h      # deduce 双端共享：summary/closure/matches 输出
├── docs/
│   └── commands.md           # 命令手册：挖掘与规则查看 CLI 参考
├── tools/
│   ├── prepare_finreflectkg.py  # FinReflectKG parquet -> TripleGraph 二进制
│   ├── prepare_triples.py    # 通用三元组转换（tsv/nt/ogb-biokg -> TripleGraph）
│   └── prepare_dbpedia_literals.py  # DBpedia literals -> 顶点属性列 TSV
├── examples/                 # load_rule / rule_set_info / matrixgraph_bridge
├── data/
│   ├── rules/example/        # 手写示例规则（rule.yaml + csr_*.bin）
│   ├── synth/                # 合成测试图（已知答案的回归用例）
│   ├── finreflectkg_s2/      # FinReflectKG 2 分片子集（TripleGraph）
│   ├── finreflectkg_s2_rules.yaml  # 该子集挖出的 1340 条规则
│   └── paper.pdf             # 目标论文（policy-guided deduction）
└── third_party/MatrixGraph/  # 图引擎（含 gflags/oneTBB/yaml-cpp 子模块）
```

## 构建

完整的挖掘/查看命令参考见 **[docs/commands.md](docs/commands.md)**。

依赖：CMake ≥ 3.20，GCC 12，CUDA ≥ 12（MatrixGraph 需要）。

```bash
git clone https://github.com/hsiaoko/MatrixGraph third_party/MatrixGraph
(cd third_party/MatrixGraph && git submodule update --init)

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

选项：

- `-DGAR_ENABLE_MATRIXGRAPH=ON/OFF`：是否启用 MatrixGraph（CUDA）集成；OFF 时仅构建纯 CPU 的 `gar_core`/`gar_miner`
- `-DGAR_BUILD_EXAMPLES=ON/OFF`

## 规则格式

一条 GAR 规则 `R = (Q[x̄], X -> p₀)` 由 YAML 表示（拓扑可用外部 CSR 二进制或内联边）：

```yaml
rule:
  id: "gar_example_001"
  labels: {Person: 1, Company: 2, works_at: 10}   # label 名字 -> LabelID 字典
  pattern:
    nodes:
      - {id: 0, var: "x", label: "Person"}
    edges:                    # 内联拓扑（有 csr_*.bin 时以二进制为准）
      - {src: 0, dst: 1, label: "works_at"}
    pivot: [0]                # support 计数支点变量（缺省取 p₀ 的变量）
  predicates:                 # X；kind 可省略，由键推断
    - {var: 0, attr: "age", op: ">=", value: 18}                 # const: x.A⊕c
    - {lhs: {var: 0, attr: "city"}, op: "=", rhs: {var: 2, attr: "name"}}  # var: x.A⊕y.B
    - {src: 0, dst: 1, label: "works_at"}                        # edge: l(x,y)
    - {var: 0, attr: "phone"}                                    # exists: x.A
    - {model: "M_anom", vars: [0, 1], label: "anom", threshold: 0.8}  # ml: M(x,y,l)
  consequent:                 # p₀，单个谓词；省略则为求值查询 ψ = Q[x̄](X)
    {src: 0, dst: 2, label: "HighRisk"}
  quality:                    # 挖掘阶段回填
    {support: 1234, confidence: 0.87, dataset: "...", miner: "..."}
```

规则存储为**一条 GAR 一个目录**：`<root>/gar_000001/rule.yaml + csr_offsets.bin +
csr_indices.bin [+ csr_edge_labels.bin]`（`mine_rules` 的输出布局，`RuleLoader::Load(dir)`
的逆操作）。也支持单文件规则集（顶层 `rules:` 序列，供 `RuleLoader::LoadRuleSet`）。

## 规则挖掘（v1：链式关系规则）

挖掘形式：`r1(x0,x1) ∧ ... ∧ rk(xk-1,xk) → rh(x0,xk)`（AMIE 式闭链，k ≤ 2，
自动加入逆关系槽位），support = 满足 body∧head 的不同 (x0,xk) pivot 对数，
confidence = support / body 对数。

```bash
# 1. FinReflectKG -> TripleGraph 二进制（字典编码 + 去重）
python3 tools/prepare_finreflectkg.py \
    --src /path/to/FinReflectKG/data --out /ssd_data/zhuxk/graphs/KG/finreflectkg/triple_graph

# 2. 装载到 MatrixGraph（ImmutableCSR + 属性表 + 字典，自包含目录）
./build/bin/kg_loader convert \
    --graph /ssd_data/zhuxk/graphs/KG/finreflectkg/triple_graph \
    --out /ssd_data/zhuxk/graphs/KG/finreflectkg/immutable_csr

# 3. 挖掘：输入图 G（--csr）+ support/confidence 阈值，输出规则目录 + 每条简述
./build/bin/mine_rules --csr /ssd_data/zhuxk/graphs/KG/finreflectkg/immutable_csr \
    --out /ssd_data/zhuxk/graphs/KG/finreflectkg/rules --min-supp 50 --min-conf 0.1

# 4. 查看（可传规则根目录或单条规则目录）
./build/bin/rule_set_info /ssd_data/zhuxk/graphs/KG/finreflectkg/rules 10
./build/bin/load_rule /ssd_data/zhuxk/graphs/KG/finreflectkg/rules/gar_000001
```

`mine_rules` 的输入二选一：`--graph`（TripleGraph 原始二进制）或 `--csr`
（MatrixGraph ImmutableCSR 目录，**推荐**——与后续 MatrixGraph 计算共用同一份图，
两种输入已验证产出完全一致的规则集）。`--dataset` 记录数据集的**根路径**
（写入每条规则的 `quality.dataset` 溯源字段，缺省取输入图的路径）。挖掘完成后
按 confidence 降序打印每条规则的一行简述（`--print N` 控制条数，0 关闭）。

其他挖掘参数：`--max-body-len`（链长，**当前实现支持 k∈{1,2}**）、
`--min-body-len`（body 最小边数，默认 1；设为 2 可排除单边"关系蕴含"规则
`r(x,y)→s(x,y)`——它语义合法但在图推理中不含拓扑结构）、`--pair-cap`
（单链 pivot 对数上限，超出记为下界）、`--no-inverses`（不用逆关系）、
`--no-typed`（关闭类型化模式，见下）。实现内还有 join 工作量上限
（`MinerConfig::join_attempt_cap`，默认 50M，防止 hub 顶点导致的组合爆炸，
超出同样记为下界），暂未开放为 CLI 参数。

**类型化模式（typed patterns）**：图带顶点 label 时（如 ogbl-biokg 的 5 类实体，
见 `kg_loader convert --label-from-prefix`），挖掘自动把链匹配按顶点 label 签名
`(t(x0),...,t(xk))` 分桶，每个桶产出独立的带类型约束规则（写入规则的
`node_labels`/`node_label_names`，label 名字进 `labels:` 字典）。均匀 label 的图
（fb15k-237 等）自动退化为无类型挖掘，产出与旧版完全一致。

性能与扩展性：挖掘过程按链并行（原子计数工作窃取，线程数 = 硬件核数），链间
完全独立、输出与单线程逐条一致。两道**精确**剪枝保证大词汇量 KG 可行：
(1) 关系频次预筛——链的 body 对数 ≤ 链上最小槽位的对数，达不到 min-supp 的
槽位不进任何链（finreflectkg 全量 30,535 关系在 min-supp 500 下只剩 379 个，
候选链 48 万条）；(2) head 频次预筛——supp(rule) ≤ |pairs(rh)|，同理跳过。
内存上不为每个关系槽位物化 n+1 的 CSR offsets（那是 slots×n 的平方级开销，
finreflectkg 会到 605GB），邻接查询直接在按 (src,dst) 排序的 pair 数组上二分。
参考耗时（104 核）：fb15k-237（2.8 万链）约 7 秒，ogbl-biokg 类型化约 3.5 分钟，
finreflectkg 约 10 分钟。

## 规则挖掘（v2：任意连通拓扑）

`mine_graph_rules`（PatternMiner）把 body 从链推广到**任意小的连通拓扑**——星型、
三角、带环结构等，挖掘形式为 `body(任意连通模式) → rh(xu,xv)`，其中 pivot 对
(u,v) 取模式上距离最远的顶点对（链时正好退化为两端点，与 ChainMiner 口径一致）：

```bash
./build/bin/mine_graph_rules --csr /ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr \
    --out /ssd_data/zhuxk/graphs/KG/fb15k-237/rules_g --min-supp 50 --min-conf 0.3 \
    --max-edges 3
```

实现要点：

- **逐层扩展**（k = 1..`--max-edges`，默认 3）：每个存活模式通过*生长*（已有顶点
  拉边到新顶点）与*闭合*（两个已有顶点间加边，产生三角/环）生成候选，按
  canonical form（顶点全排列取字典序最小）去重，Apriori 剪枝（任一有序顶点对的
  投影对数 < min-supp 即淘汰）。`--min-body-edges`（默认 1）控制出规则的最小
  body 边数——设为 2 则排除单边规则，但 k=1 模式仍作为父模式参与扩展。
- **槽位共现索引**预筛：新边在某个模式顶点上必须能与该顶点已有的每条边在同一
  实体上共现，否则候选直接不生。
- **计数复用 ChainMiner 口径**：单边/两链模式的计数走与 ChainMiner 完全相同的
  嵌套循环 join（含同样的 cap），**k ≤ 2 时两个挖掘器产出逐字节一致的规则集**
  （fb15k-237：1,652 条，name/support/confidence 全等对账通过）；k ≥ 3 的通用
  拓扑用 `PatternMatcher`（贪心匹配序 + 二分邻居交的回溯枚举）计数。
- 同一规则可能从多个 (模式, pivot) 组合产出（链与其反向走步是两个 canonical
  模式），按规则名去重后写出。
- 输出格式与 ChainMiner 完全相同（一规则一目录），下游 `match_pattern` /
  `mine_predicates` / `rule_set_info` 直接可用——`mine_predicates` 的匹配枚举
  同样已换用 `PatternMatcher`，支持任意拓扑规则（在 dbpedia 166 条带 X 规则上
  与旧的链式枚举逐字节一致）。

实证（fb15k-237，min-supp 50 / min-conf 0.3，104 核）：`--max-edges 3` 评估
766 万个模式约 29 分钟，产出 81,683 条规则——其中 k≤2 子集 1,652 条与
ChainMiner 完全一致，k=3 新增 30,055 条三边链、43,108 条星型（4 顶点）、
6,868 条三角（3 顶点带环）规则。注意 k=3 的候选规模是 k=2 的 ~140 倍
（5.4 万 → 760 万），更大的图建议保持 `--max-edges 2` 或提高 min-supp。

## KG 装载到 MatrixGraph（kg_loader）

把统一格式的三元组图（TripleGraph 目录）装进 MatrixGraph 的 `ImmutableCSR`
（走 MatrixGraph 自己的 `Edges` → `Edgelist2ImmutableCSR` → `ImmutableCSR::Write`
管线，保证二进制布局与引擎完全一致）：

```bash
# 转换：TripleGraph -> ImmutableCSR + 属性表
./build/bin/kg_loader convert --graph <triple_graph_dir> --out <csr_dir> [--label-from-prefix]

# 展示：用 ImmutableCSR::Read 读回，打印元数据、label 分布、属性 schema、样例顶点
./build/bin/kg_loader show --csr <csr_dir> --print 3
```

输出目录布局（拓扑与属性分离，行序对齐契约与 GraphRAG 一致）：

```
<csr_dir>/
├── meta.yaml / graphs/0.bin / label/0.bin   # MatrixGraph 原生三件套（拓扑 + 顶点 label）
├── vertex_attr/    # 顶点属性表（entity_id=0）：row i == 顶点 local id i
│   └── meta.yaml + <col>.bin                #   如 type:Int[n]（--label-from-prefix 时）
└── edge_attr/      # 边属性表（entity_id=1）：row i == outgoing_edges 槽位 i
    └── meta.yaml + relationship.bin         #   relationship:Int[m]，关系字典 id
```

- **属性 IO 在 MatrixGraph 侧**：`core/io/attributes_io.cuh/.cu`（`AttributesIO::Write/Read`），
  序列化 MatrixGraph 自己的列式 `Attribute` 结构（10 种 ValueType，标量/列表/字符串），
  补齐了 `data_structures/attributes.h` 原本缺失的磁盘 IO，与 `core/io/` 其他 IO 模块同风格。
- **边序对齐**：MatrixGraph 对同一 src 的出边按 dst 排序（`format_converter.cuh` 的
  `std::sort`），因此 slot 序 = 输入三元组的稳定 (src,dst) 排序；同一 (src,dst) 的
  多重边在其槽位组内可互换。convert 按此顺序写 `edge_attr`，已用独立脚本逐点对账。
- `--label-from-prefix`：按实体名 `:` 前缀（如 `drug:CID...` → `drug`）生成顶点 label
  （写入 label/0.bin）、`vertex_attr/type` 列和 `dict/labels.tsv`（label id → 名字），
  适用于 ogbl-biokg 这类带类型的图谱。
- 字符串（实体名/关系名）不进属性表，保留在 TripleGraph 字典层（`entities.tsv` /
  `relations.tsv`），跨语言只传 uint32 id——与 GraphRAG 的契约一致。

## 模式匹配与匹配表（match_pattern）

挖掘的第二阶段：把某条规则的模式 Q 回放（replay）到图上，**复用 MatrixGraph 的
GPU 任务 `task::GARMatch`**（`core/task/gpu_task/gar_match.cu`，边 label + 顶点 label
约束、有向、单射匹配），把匹配物化为 `GARMatchArrays` 关系表（行 = (pivot, 模式位置)，
值去重）——这张表就是在其上挖属性谓词的"关系表"：

```bash
./build/bin/match_pattern --csr <csr_dir> --rule <rule_dir> [--attr-stats] [--print 10]
```

- 输入直接复用管线产物：`--csr` 是 kg_loader 的输出目录（拓扑 + label/0.bin +
  edge_attr/relationship），`--rule` 是 mine_rules 输出的单个规则目录。
- 规则里的逆关系槽位（`r^-1`）在调用前折算回基关系（交换 src/dst）。
- `--attr-stats`：用 `AttributesIO::Read` 读 `vertex_attr/` 的 Int 列，在匹配表上统计
  const 谓词候选 `x{i}.<列> = c` 及其 pivot 加权占比。
- **匹配语义是单射**（模式顶点两两不能映到同一实体，GARMatch kernel 的
  `GARContainsAssigned` 保证），而 ChainMiner 计数用同态语义——同一条规则的
  匹配数会略小于挖掘时的 body 对数，差异来自仅含重叠见证的 pivot 对。
- 容量：`--row-cap` / `--match-cap` 控制输出行数与值池上限（到达上限会截断并告警）。
  GARMatch 的 join 是逐边候选的嵌套循环展开，适合"已挖出的规则"级别的回放，
  不适合挖掘期的大规模计数（挖掘计数仍在 CPU ChainMiner 完成）。

## 属性谓词挖掘（mine_predicates，阶段三）

有了真实顶点属性后（如 DBpedia literals），在已挖出规则的匹配上挖 `const`
（`x.A = c`）与 `var`（`x.A = y.A`，同一属性跨变量）谓词回填 X：

```bash
# 1. literals -> 属性列 TSV（数值/年份，按覆盖率取 top-N）
python3 tools/prepare_dbpedia_literals.py \
    --ttl mappingbased_literals_en.ttl \
    --graph /ssd_data/zhuxk/graphs/KG/dbpedia/triple_graph \
    --out /ssd_data/zhuxk/graphs/KG/dbpedia/literal_attrs --top 60

# 2. 属性列并入 CSR 目录（AttributesIO 写入，稀疏列带 valid 掩码，
#    可与已有列合并；kg_loader show 可查看）
./build/bin/kg_loader add-vertex-attrs \
    --csr /ssd_data/zhuxk/graphs/KG/dbpedia/immutable_csr \
    --attrs /ssd_data/zhuxk/graphs/KG/dbpedia/literal_attrs

# 3. 谓词挖掘：逐规则重枚举 body 匹配（CPU PatternMatcher，任意拓扑，
#    与挖掘器同口径的同态语义），贪心加入使 confidence 严格提升的谓词
#    （support 仍用 pivot 对口径）
./build/bin/mine_predicates --csr /ssd_data/zhuxk/graphs/KG/dbpedia/immutable_csr \
    --rules /ssd_data/zhuxk/graphs/KG/dbpedia/rules \
    --out /ssd_data/zhuxk/graphs/KG/dbpedia/rules_enriched \
    --min-supp 500 --min-conf 0.3
```

产出的规则 X 非空（`predicates:` 写入 rule.yaml），`quality` 是在 `body∧X` 下
重算的 support/confidence，`description` 记录无 X 时的基线。匹配数超过
`--max-matches`（默认 2M）的规则跳过，避免截断统计失真。

**反指纹过滤**：取值只落在单个实体上的谓词（如 `populationTotal = 66736000`
锁定 x2=法国）会让 conf 平凡地变 1——const 候选要求取值覆盖 ≥2 个不同实体，
var 候选要求等值组两侧都 ≥2 个实体。

## 演绎（deduce，逻辑闭包）

给定规则集目录与 KG，做完整的 **chase**（CPU `engine::deduction::CpuChaseEngine` / GPU
`engine::deduction::GpuDeduceEngine`（MatrixGraph `task::GARDeduce`），统一入口
`gar_reason --mode full --engine cpu|gpu`，旧 app `deduce` / `deduce_gpu` 为薄壳）：

```
G_{i+1} = G_i ∪ { p₀(emb) | 规则 Q[x̄](X → p₀)，emb ∈ Q(G_i)，X(emb) 成立 }
```

迭代至不动点（head 不引入新顶点，闭式有界必终止；`--max-rounds` /
`--max-derivations` / `--max-matches` 为实用上限，触发则标记非不动点）。
拓扑匹配为同态语义（边 label、顶点 label 约束），X 谓词
（const/var/exists/edge）逐 embedding 求值——**这是目前唯一 X 生效的环节**
（`match_pattern` 的 GPU GARMatch 只做拓扑+label）。推出的边按轮次合并回
TripleGraph（含逆槽位镜像），后续轮次在增强图上继续匹配，支持跨规则链式推导。

```bash
./build/bin/deduce --csr /ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr \
    --rules /ssd_data/zhuxk/graphs/KG/fb15k-237/rules \
    --out /tmp/fb_closure [--max-rounds 100] [--max-derivations 10000000] \
    [--max-matches 1000000] [--no-matches] [--print 20]
# GPU 版：参数与输出布局完全相同
./build/bin/deduce_gpu --csr ... --rules ... --out /tmp/fb_closure_gpu [...]
```

输出：`<out>/summary.yaml`（逐规则 embeddings/x_satisfied/derived + 总轮数与
不动点标记）、`<out>/closure_edges.tsv`（推出的新事实：src/relation/dst/推导轮次/
来源规则）、`<out>/matches/<rule>.tsv`（末轮全部匹配，含 x_ok/head 标记，
`--no-matches` 关闭）。实证：synth 上 3 轮推出 9 条新事实（第二轮用上了第一轮
推出的边，跨轮链式推导正确）；dbpedia 带 X 规则 `countySeat^-1∧{areaWater=0} →
isPartOf` 上 3016 拓扑匹配 → 1137 过 X → 1107 已在图中（恰为挖掘 support）+ 30 条
新事实，谓词计数与挖掘期口径一致。

**GPU 版架构**（`third_party/MatrixGraph` 新增 `task::GARDeduce` +
`kernel_gar_deduce`，不改动既有文件；app 为 `apps/deduce_gpu.cu`）：
匹配/谓词求值/head 发射全在 device——每轮 `SetGraph` 上传一次边表；每规则两遍
全边扫描取候选、cub 分段排序成 (src,dst)/(dst,src) 双序键，join 用**二分搜索 +
邻接段扫描**（CPU `PatternMatcher` 有序 pair 数组的 device 对应物，非
frontier×candidates 暴力对）；X 谓词（含属性列，device 常驻缓存）与 head 发射
同 kernel 完成。**主端逻辑与 CPU 完全共享**：轮驱动、head 存在性预过滤
（`graph::MarkAbsentEdges`，TBB 并行）、按规则序去重合并
（`graph::MergeRoundFacts`）、输出文件（`apps/closure_output.h`）。

**双端一致性实证**：synth、dbpedia（166 条带 X 规则、60 列 literals）与
fb15k 全 1,652 规则 2 轮无截断（648,595 条闭包事实）上 `summary.yaml`
逐字节一致、闭包集合一致；fb15k 全闭包（10M 上限）轮数/总量/结构一致，
差异仅出现在 match 上限截断的规则上（截断子集选择顺序 CPU 枚举序 vs
GPU atomic 序，属预期行为）。**注意**：GPU 匹配为广度优先 frontier join
（frontier 常驻 16M 行），CPU 为回溯枚举；无上限时两者产出相同集合，
有上限时只有截断规则内的组成不同。

**性能注意**：CPU 侧是朴素 chase（每轮全量重枚举所有规则）；轮内规则级并行
（oneTBB，合并按规则序保证去重归属确定、输出与串行逐字节一致），轮末所有
槽位的新边一次批量并入索引（`AddEdgesBatch`，全图邻接只重建一次）。实证
（fb15k-237 全 1,652 规则、10M 事实上限）：串行 183 秒 → 并行 + 批量合并
**17.6 秒**；GPU 版同负载 **49 秒**（匹配 kernel 本身每轮仅 4-15 秒，时间
大头在每规则十余次 kernel launch/同步的固定开销——6,608 次规则调用 × 毫秒级
延迟，以及主端 head 预过滤与合并；规则批处理（多规则同 kernel）是已识别的
优化点）。dbpedia 2 轮 62,356 事实：GPU 32 秒 vs CPU 21 秒。半朴素
（semi-naive）增量匹配是双端共同的后续优化点。

## 数据集

已下载并接入管线的 4 个知识图谱（`/ssd_data/zhuxk/graphs/KG/<name>/`，均含
`triple_graph/` 与 `immutable_csr/` 两种格式）：

| 数据集 | 实体 | 关系 | 去重边 | 顶点 label | 来源 |
|---|---|---|---|---|---|
| fb15k-237 | 14,513 | 237 | 289,650 | 无 | [微软官方](https://www.microsoft.com/en-us/download/details.aspx?id=52312)（train+valid） |
| ogbl-biokg | 93,773 | 51 | 4,762,677 | 5 类（drug/protein/disease/sideeffect/function） | [OGB](https://ogb.stanford.edu/docs/linkprop/) |
| finreflectkg | 2,477,375 | 30,535 | 8,490,524 | 无 | [FinReflectKG](https://arxiv.org/pdf/2508.17906)（103 分片，17.5M 行去重） |
| dbpedia | 5,702,401 | 661 | 18,741,534 | 无（instance_types 文件已下载备用）；60 列 literals 属性已并入 `vertex_attr/` | [DBpedia 2016-10 mappings](https://downloads.dbpedia.org/2016-10/core-i18n/en/) |

已产出的规则集：`fb15k-237/rules/`（1,652 个规则目录，min-supp 50 / min-conf 0.3）、
`fb15k-237/rules_g3/`（81,683 个**通用拓扑**规则目录，`mine_graph_rules --max-edges 3`，
含星型/三角等非链规则）、
`ogbl-biokg/rules/`（56,601 个**类型化**规则目录，min-supp 200 / min-conf 0.3，
挖掘约 3.5 分钟）、`finreflectkg/rules/`（455 个规则目录，min-supp 500 /
min-conf 0.3，48 万条候选链）、`dbpedia/rules/`（607 个规则目录，min-supp 500 /
min-conf 0.3，14.8 万条候选链）、`dbpedia/rules_enriched/`（166 个**带属性谓词 X**
的规则目录，在 60 列 literals 上由 `mine_predicates` 产出，min-supp 500）、`data/finreflectkg_s2_rules.yaml`（1,340 条，
2 分片子集，旧的单文件格式，min-supp 50 / min-conf 0.1）。
用 `./build/bin/rule_set_info <root|yaml>` 查看。

新数据集接入：TSV / N-Triples / OGB-biokg 三种格式一条命令转换——

```bash
python3 tools/prepare_triples.py --format tsv --in train.txt --out <out_dir>
```

## 运行示例

```bash
./build/bin/load_rule data/rules/example          # 加载单条规则（目录形式）
./build/bin/matrixgraph_bridge data/rules/example # 转换为 MatrixGraph 的 pattern 数组布局
./build/bin/mine_rules --graph data/synth --out /tmp/synth_rules --min-supp 2 --min-conf 0.5
./build/bin/rule_set_info /tmp/synth_rules
./build/bin/kg_loader convert --graph data/synth --out /tmp/synth_csr
./build/bin/match_pattern --csr /tmp/synth_csr --rule /tmp/synth_rules/gar_000001
```

## 库目标

| CMake target | 说明 |
|---|---|
| `gar::core` | 规则表示与加载（CSR + YAML，纯 CPU） |
| `gar::graph` | 图与语义内核（TripleGraph + PatternMatcher + 谓词求值/事实合并，纯 CPU） |
| `gar::hdc` | HDC 数值内核（bit-packed 超向量：bind/bundle/permute/cosine，零依赖纯 CPU） |
| `gar::analysis` | L2 分析服务（PredicateTable + DependencyGraph/SCC + RuleSetStats + GoalSampler；仅依赖 core+graph） |
| `gar::miner` | GAR 挖掘（ChainMiner 链式 + PatternMiner 通用拓扑，纯 CPU） |
| `gar::engine::deduction` | GAR 演绎引擎家族（DeductionEngine 基类；CpuChaseEngine 朴素 chase 纯 CPU + GpuDeduceEngine） |
| `gar::engine::agent` | 策略制导演绎（ReasoningAgent + GreedyExecutor + Scorer/Policy 插件，依赖 graph/analysis/hdc/core） |
| `gar::bridge` | 到 MatrixGraph 的桥接（依赖 `matrixgraph_core`，CUDA） |

## 可执行程序

| 程序 | 用途 |
|---|---|
| `mine_rules` | 链式 GAR 挖掘（TripleGraph/ImmutableCSR → 一规则一目录） |
| `mine_graph_rules` | 通用拓扑 GAR 挖掘（星型/三角/带环，k≤2 时与 mine_rules 一致） |
| `deduce` | 演绎：规则集在 KG 上的完整逻辑闭包（CPU chase，拓扑 + X 谓词） |
| `deduce_gpu` | 演绎 GPU 版（MatrixGraph GARDeduce：匹配/X 求值/head 发射在 device） |
| `gar_reason` | 演绎统一入口（--mode full --engine cpu|gpu，输出与旧 app 逐字节一致；--threads 限 CPU 并行度） |
| `gar_analyze` | 分析工具链（rules-stats / gen-goals / gen-workload / chain-cmp / verify 三方对账） |
| `validate_rules` | 规则检验：重算 pivot-pair support/confidence 与挖掘值对账（PASS/DRIFT） |
| `kg_loader` | TripleGraph ↔ MatrixGraph ImmutableCSR 转换与 CLI 展示 |
| `match_pattern` | 规则模式回放（复用 MatrixGraph GPU GARMatch）→ 匹配表 + 属性谓词统计 |
| `mine_predicates` | 阶段三：规则匹配上的 const/var 属性谓词挖掘（X 回填） |
| `load_rule` | 加载单条规则（目录形式）并打印 |
| `rule_set_info` | 加载规则集目录/YAML 并打印摘要 |
| `matrixgraph_bridge` | 规则 → MatrixGraph `GARPatternArrays` 布局演示 |
