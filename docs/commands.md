# 命令手册

GARAgent 的完整 CLI 参考。管线三阶段 + 双推理入口：

```
mine_rules（ChainMiner，链式）──┐
mine_graph_rules（PatternMiner，任意拓扑）┘→ match_pattern（GPU 回放）→ mine_predicates（X 谓词）
                                            ↓ 规则集
                    gar_reason（演绎统一入口：--mode full|agent）
                    gar_analyze（分析工具链：rules-stats/gen-goals/gen-workload/chain-cmp/verify）
```

所有挖掘器输入二选一：`--csr`（MatrixGraph ImmutableCSR 目录，**推荐**，与下游
计算共用同一份图）或 `--graph`（TripleGraph 原始二进制）。输出均为**一规则一目录**
（`gar_000000/{rule.yaml, csr_offsets.bin, csr_indices.bin, csr_edge_labels.bin}`）。

## 1. 挖掘

### 链式规则（mine_rules）

```bash
./build/bin/mine_rules \
    --csr /ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr \
    --out /ssd_data/zhuxk/graphs/KG/fb15k-237/rules \
    --min-supp 50 --min-conf 0.3
```

| 参数 | 默认 | 说明 |
|---|---|---|
| `--min-supp N` | 50 | support 阈值（满足 body∧head 的不同 pivot 对数） |
| `--min-conf F` | 0.05 | confidence 阈值（support / body 对数） |
| `--max-body-len K` | 2 | 链长上限（当前实现 k∈{1,2}） |
| `--min-body-len K` | 1 | 链长下限；2 = 排除单边关系蕴含规则 `r(x,y)→s(x,y)` |
| `--pair-cap N` | 4M | 单链 pivot 对数上限（超出记为下界） |
| `--no-inverses` | 开 | 不用逆关系槽位 |
| `--no-typed` | 开 | 关闭类型化模式（图带顶点 label 时） |
| `--dataset PATH` | 输入路径 | 数据集根路径溯源（写入 `quality.dataset`） |
| `--print N` | 20 | 挖完按 confidence 降序打印前 N 条简述，0 关闭 |

### 通用拓扑规则（mine_graph_rules）

body 为任意连通小拓扑（链、星型、三角、带环）。k≤2 时与 `mine_rules`
产出逐字节一致；k=3 起产生非链规则。

```bash
./build/bin/mine_graph_rules \
    --csr /ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr \
    --out /ssd_data/zhuxk/graphs/KG/fb15k-237/rules_g3 \
    --min-supp 50 --min-conf 0.3 \
    --max-edges 3 \
    --min-body-edges 2        # 可选：只要结构性规则（body ≥ 2 条边）
```

| 参数 | 默认 | 说明 |
|---|---|---|
| `--max-edges K` | 3 | body 最大边数；注意 k=3 候选约是 k=2 的 140 倍，大图建议 2 |
| `--min-body-edges K` | 1 | 出规则的最小 body 边数（k<K 的模式仍作父模式参与扩展） |
| `--match-cap N` | 2M | 单模式 embedding 枚举上限（超出记为下界） |
| `--pair-cap` / `--min-supp` / `--min-conf` / `--dataset` / `--print` | 同上 | 同上 |

参考耗时（fb15k-237，supp50/conf0.3，104 核）：k≤2 约 21 秒；k=3 约 29 分钟
（766 万模式 → 81,683 条规则：30,055 三边链 + 43,108 星型 + 6,868 三角 +
1,652 k≤2）。

### 谓词挖掘（mine_predicates，阶段三）

在已挖出规则的匹配上挖属性谓词回填 X（需图带 `vertex_attr/`，如 dbpedia 的
60 列 literals）。支持任意拓扑的规则目录（mine_rules / mine_graph_rules 的产物均可）。

```bash
./build/bin/mine_predicates \
    --csr /ssd_data/zhuxk/graphs/KG/dbpedia/immutable_csr \
    --rules /ssd_data/zhuxk/graphs/KG/dbpedia/rules \
    --out /ssd_data/zhuxk/graphs/KG/dbpedia/rules_enriched \
    --min-supp 500 --min-conf 0.3
```

| 参数 | 默认 | 说明 |
|---|---|---|
| `--max-matches N` | 2M | 单规则匹配枚举上限（超出的规则跳过，避免截断统计失真） |
| `--max-preds K` | 2 | 每条规则最多加入的谓词数（贪心，confidence 严格提升才加） |

## 2. 查看与回放

```bash
# 单条规则：pattern CSR（ImmutableCSR 风格逐顶点邻接）+ dependency（X→Y）完整展示
./build/bin/load_rule /ssd_data/zhuxk/graphs/KG/fb15k-237/rules_g3/gar_011860

# 规则集：摘要 + 前 N 条（缺省 N=10）
./build/bin/rule_set_info /ssd_data/zhuxk/graphs/KG/fb15k-237/rules_g3 5

# 规则回放到图上：GPU GARMatch 物化匹配表（单射语义）
./build/bin/match_pattern \
    --csr /ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr \
    --rule /ssd_data/zhuxk/graphs/KG/fb15k-237/rules_g3/gar_011860 \
    --print 10
```

**提示**：规则目录编号按挖掘层序排列，`gar_000000` 起是单边规则。看多边规则
（星型/三角）传大一点的 N，或直接打开靠后的编号。按 body 边数过滤：

```bash
python3 - <<'EOF'
import struct, glob
for d in sorted(glob.glob('<rules_root>/gar_*')):
    with open(d+'/csr_offsets.bin','rb') as f: b=f.read()
    off = struct.unpack(f'{len(b)//4}I', b)
    if off[-1] >= 3: print(d)   # body ≥ 3 条边的规则
EOF
```

## 3. 演绎统一入口（gar_reason）

一个二进制两种模式：`--mode full`（全闭包 chase，双引擎）与
`--mode agent`（策略制导演绎，论文 §3-§5）。

### 3.1 full 模式：全闭包（chase）

```bash
./build/bin/gar_reason --mode full --engine cpu \
    --csr /ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr \
    --rules /ssd_data/zhuxk/graphs/KG/fb15k-237/rules \
    --out /tmp/fb_closure --threads 0
```

| 参数 | 默认 | 说明 |
|---|---|---|
| `--engine cpu\|gpu` | 必填 | CpuChaseEngine（oneTBB）/ GpuDeduceEngine（GARDeduce） |
| `--max-rounds N` | 100 | chase 轮数上限（head 不引新顶点，理论必终止） |
| `--max-derivations N` | 10M | 推出新事实总数上限 |
| `--max-matches N` | 1M | 单规则单轮 embedding 上限（超出记 capped，统计为下界） |
| `--no-matches` | 关 | 不写末轮匹配文件（省磁盘） |
| `--threads N` | 0 | CPU 引擎 TBB 并行上限，0=全核（GPU 引擎忽略） |
| `--print N` | 20 | CLI 按 derived 降序打印前 N 条规则 |

输出（`<out>/`）：`summary.yaml`（逐规则末轮 embeddings/x_satisfied/derived +
总轮数/不动点标记，以及全轮累计 `embeddings_total`、`x_satisfied_total`、
`derivation_candidates_total`；最后一个计数是轮内去重前候选，不等于新事实数）、
`closure_edges.tsv`（新事实：src、relation、dst、推导轮次、来源规则）、
`matches/<rule>.tsv`（末轮全部匹配 + x_ok/head 标记）。

**兼容**：旧 app `deduce` / `deduce_gpu` 仍是独立二进制，CLI 与输出**逐字节
等价**于 `gar_reason --mode full --engine cpu|gpu`（薄壳共享
apps/full_deduce_cli.h + 引擎库；回归对 .baseline 逐字节一致）。

实证：synth 3 轮推出 9 条（含跨轮推导）；dbpedia 带 X 规则验证谓词计数与挖掘期
口径一致（x_ok − derived = 挖掘 support）；fb15k 全 1,652 规则 10M 事实上限 4 轮
**17.6 秒**（轮内 oneTBB 规则级并行 + `AddEdgesBatch` 批量索引合并；输出与串行
逐字节一致）。GPU 版：匹配/X 求值/head 发射在 device（候选 cub 分段排序 +
二分搜索邻接段扫描 join，属性列 device 常驻），主端共享 CPU 的轮驱动/预过滤/
合并/输出。双端对账：synth、dbpedia 2 轮、fb15k 2 轮无截断（648,595 事实）均
`summary.yaml` 逐字节一致 + 闭包集合一致；有 match 截断时仅截断规则内组成不同
（子集选择顺序差异，预期行为）。耗时：fb15k 全闭包 GPU 49 秒 vs CPU 17.6 秒；
dbpedia 2 轮 GPU 32 秒 vs CPU 21 秒。

### 3.2 agent 模式：策略制导演绎

目标制导反向演绎（backward，主引擎）、单链朝目标事实演绎（greedy）或带证书
的最优链搜索（bnb，Figure 6 HybNS，可选 HDC 价值模型排序，见 §3.3）：

```bash
./build/bin/gar_reason --mode agent --csr <immutable_csr_dir> --rules <rules_root> \
    --goal 'rel_name(src,dst)' --budget 30 \
    [--policy deterministic|random|hybrid|jev|q-hdc|q-numeric] [--scorer exact|hdc|null] [--hdc-dim 8192] \
    [--executor greedy|bnb|backward|fixpoint] [--direction forward|backward] [--top-k N] [--seed 42] [--policy-seed N] [--hdc-seed N] [--max-subgoal-depth N] [--max-candidates-per-rule 256] \
    [--bnb-priority utility|hdc-value|numeric-value] [--backward-priority heuristic|hdc-value] [--value-model PATH] \
    [--hdc-value-backend original|simd|bit-lut] [--no-dg] \
    [--fair-interval N] [--fixpoint-star-order input|goal] [--fixpoint-q-trace PATH] \
    [--backward-order score|random|hybrid] [--open-collect-cap N] \
    [--alpha 1] [--beta 1] [--gamma 0.5] [--nu 1] [--nu2 0.5] [--tau-hdc 0.15] \
    [--no-confidence] [--out <dir>] [--verify-against-closure] [--print 20]
```

| 参数 | 默认 | 说明 |
|---|---|---|
| `--goal 'rel(s,d)'` | 必填 | 目标事实；实体名，`_` = 开放变元 |
| `--budget N` | 100 | 单链成本上限（unit cost：一次应用 = 1，成败都计费） |
| `--policy` | deterministic | argmax / softmax(total/τ)（`--seed` 可复现）/ admissible 硬过滤+argmax / **jev**（LLM 决策模型，§5.3）/ **q-hdc、q-numeric**（action-level cost-to-go，仅限 fixpoint+exact，需 `--value-model`，只改排序） |
| `--policy-seed` / `--hdc-seed` | 回退 `--seed` | 种子解耦：policy 侧 RNG（random policy、backward-order 洗牌、jev mock）用 policy-seed；HDC encoder 用 hdc-seed；缺省均回退 --seed（旧行为逐字节不变）。summary.yaml 记录三者 |
| `--max-subgoal-depth` | 4 | backward/backward-bnb 子目标递归深度上限（默认 4 保持旧命令不变） |
| `--scorer` | exact | exact（见证前提比例）/ hdc（cos(v_μ,E) 证据向量读数）/ null（消融：无打分，仅保留符号 admissible 过滤，greedy 退化为候选枚举序，bnb 退化为 uniform-cost） |
| `--executor` | greedy | backward（目标制导反向演绎，主引擎）/ greedy（增量 forward：policy 前置选规则 + 候选 cache + delta 匹配，§5.1）/ bnb（Figure 6 分支定界）/ fixpoint（挂起-复活两循环，见下） |
| `--fair-interval` | 0（关） | fixpoint 执行器：每 N 次 policy 选择强制执行等待最久的 ready action（trace 记 fair_select）；0=纯 policy 序 |
| `--fixpoint-star-order` | input | fixpoint 的 star 展开顺序：input=规则目录序（保留旧调度）；goal=按结论谓词到 goal 的 DG 距离稳定排序，同距离保持目录序。无提前终止时两者都展开全部 goal-cone star |
| `--fixpoint-q-trace PATH` | 空（关） | fixpoint+exact 下写出每个实际求值 action 的 11 个特征；goal 命中时 `remaining_evaluations` 为含当前 action 的剩余求值数，未命中为 0。仅训练用，未观察 alternative 不伪造标签 |
| `--direction` | 不设 | 推理方向开关：不设 = 旧行为（`--executor` 为准，逐字节不变）；`forward` 下 `--executor backward` 互斥报错；`backward` = 反向纪律（`--executor backward` 为完整 DFS+回溯，`--executor greedy` 为退化单路径消融臂——首选即终选、不回溯；`--executor bnb` 报错，留待 Phase 2 的 AND-OR 证书） |
| `--backward-priority` | heuristic | backward 的 OR 候选（规则尝试）排序：heuristic（tier/open_d/rel_fam/conf，旧行为）/ hdc-value（HDC 价值模型预测失败率，只改排序，需 `--value-model`） |
| `--backward-order` | score | BackwardExecutor 候选排序：score（打分序，默认逐字节不变）/ random（`--seed` 洗牌，打分不参与排序）/ hybrid（top-8 打分窗口内洗牌，其余按分） |
| `--open-collect-cap` | 8 | 开放变元 target 单次收集答案数上限；求全外延时调大 |
| `--backward-full-enum` | 关 | 开放变元 goal 全外延模式：pass1/pass2 扫描循环至 journal 不动点（已知结论跳过、失败备忘录随图增长重准入）；默认关 = 旧行为逐字节不变 |
| `--forward-full-enum` | 关 | greedy 对开放变元 goal 的全外延模式：不在首个答案处停，继续步进至候选耗尽（fixpoint）/budget/墙钟；默认关 = 旧行为逐字节不变 |
| `--wall-limit S` | 0（关） | 执行器内墙钟上限（秒）：到时干净终止（termination=timeout）并照常写出部分结果；外部 kill 会丢数据 |
| `--bnb-priority` | utility | bnb 优先级来源：utility（原公式）/ hdc-value（HDC 价值模型，需 `--value-model`，只改排序，见 §3.3）/ numeric-value（同 11 原始特征+同 4 交互项的数值 ridge，标准化参数只从训练集拟合；同样只改排序） |
| `--value-model PATH` | 无 | HDC/numeric 价值模型的 `.meta` 路径，同名 `.bin` 保存向量与权重 |
| `--hdc-value-backend` | bit-lut | HDC 价值模型计算后端：original=原始稠密计算；simd=在线 AVX2 bundle/bind/dot；bit-lut=加载期 bit-packed XOR + SIMD 预计算有限分箱词表的加权点积，在线仅查表求和。三者数学模型相同，summary 记录后端 |
| `--no-dg` | 关 | 关闭 DG 制导（d(·) 全零：不剪枝、rel_fam 中性、price=w₀）；消融用 |
| `--top-k N` | 0 | bnb 每分支候选截断；0=精确模式（带证书） |
| `--hdc-dim N` | 8192 | HDC 维度（档位 2 的幂，128…16384） |
| `--max-candidates-per-rule` | 256 | 每次枚举调用上限：bnb=每分支全候选展开；增量 greedy=规则的初始全量枚举一次 + 每次 delta 枚举（每个匹配新事实 slot 的 pattern edge 一次调用）。cache 跨 delta 累计，规则候选总数可超过该值；任何一次枚举触顶即计入 fwd_capped_enums，引擎不声称完备 |
| `--alpha/--beta/--gamma/--nu/--nu2/--tau-hdc` | 1/1/0.5/1/0.5/0.15 | 打分权重（total=α·(rel_fam+ν·rel_evd+ν2·conf)−β·dcost） |
| `--verify-against-closure` | 关 | app 层另跑 CpuChaseEngine 全闭包，断言 agent 导出 ⊆ 闭包（soundness 写入 summary.yaml） |
| `--jev-endpoint URL` | http://127.0.0.1:8080 | Jev System One 本地端点（纯 HTTP，超时 `--jev-timeout-ms` 默认 2000ms） |
| `--jev-top-k N` | 8 | 每步送 Jev 排序的候选数（按 total 截取，admissible 硬过滤前置） |
| `--jev-sample` | 关 | 按 Jev 返回的 π 采样（固定种子可复现）；默认 argmax π |
| `--jev-mock` | 关 | 用内置 MockJevClient（无网络，按 witnessed 比例+定值噪声出概率），供无 Jev 环境开发/回归 |

jev 说明：decision model 只排序不发明（"rank, not invent"），候选与全部逻辑
校验都在符号侧；调用计 control overhead（墙钟，不进预算 B），
summary.yaml 的 `control_overhead{calls, wall_ms, fallbacks}` 记录。HTTP
异常/超时/解析失败时警告一次并回退 argmax total（与 deterministic 逐字节一致）。
与 bnb 组合时打分由 scorer 承担，Jev 不产生调用（calls=0）。

输出（`--out`）：`chain.tsv`（每步 rule/embedding/成败/成本/导出事实）、
`closure_edges.tsv`（与 full 同布局）、`summary.yaml`（goal/derived/成本/
终止原因/达成 + bnb 的 certified_optimal/certified_no_chain/anytime_fallback/
utility/expansions；`first_goal_expansions` 和 `first_goal_wall_ms` 记录首个解的
展开数与执行器内耗时（未找到 goal 时分别为 0/-1），最终证书搜索仍继续至穷尽；
greedy 的 fwd_* 增量引擎计数：`fwd_rule_selections`
（policy 规则选择次数）、`fwd_rules_initialized`（首次全量枚举规则数）、
`fwd_delta_enums`（delta 枚举次数）、`fwd_embeddings_generated`（枚举产出
embedding 总数）、`fwd_candidates_reused`（cache 复用候选数）、
`fwd_capped_enums`（触及 match cap 的枚举次数 = 截断明示）、
`fwd_match_wall_ms`/`fwd_apply_wall_ms`/`fwd_score_wall_ms`（分项耗时））。

**greedy = 增量 forward 引擎**（2026-10-31 起替换了旧的"每步全规则重
枚举"naive 实现，后者已删除、无回退开关；历史实验
（experiments/forward_variants、open_full 的 full_matrix fwd_* 行）是
该已移除实现的结果，仅作基线）：两阶段 policy——先按规则级静态分
（rel_fam/conf/price，无需 embedding）选规则，只匹配选中的规则；候选
cache 持久化（首次全量、后续按新事实 delta 增量匹配，pin 住新事实两端
点枚举含该边的 embedding 并去重合并）；goal 常量只 pin 直接产生 goal
关系的规则头（不做多跳 Magic Sets）。终止语义：`no_candidates` = 全部
可达规则已初始化且 cache 在当前 fact version 耗尽（cap 截断空间内的
fixpoint，截断由 fwd_capped_enums 明示，不声称完备）；`timeout` =
--wall-limit 到期（部分结果照常落盘）。开发校验：
`GAR_VERIFY_INCREMENTAL=1` 时每次 delta 合并后对全部触及规则逐一全量重枚举比对
（不一致即 abort，仅小图/小预算使用）。

**fixpoint = 挂起-复活两循环执行器**（docs/fixpoint_implementation_plan.md
的实现）：R（完整 action）只由 action policy 调度；前提逐条求值（不短路）：
全成立→精确派生；静态假→永久剪枝并经前提缓存 H 共享（H 键 = 结构化
GroundPreconditionKey：完整逻辑谓词 + ground 绑定、full equality，不同
op/常量/rhs_attr 的前提不共享）；动态缺失→连全部缺失 ground key 挂起进 W
（含 CSR pattern 内缺失动态边形成的 partial action——未绑定变量只存内部
PartialMatch，不进 Motif/Scorer/Policy）。逐 star 展开并排空其 R，delta 只投给
已展开 star；全部展开后进 fixpoint 循环；W 由 delta 或 fixpoint 循环复活，
producer 全灭才安全剪枝。goal 命中立即返回。

2026-11-05 性能与正确性修订：exact/null 缓存 action 分数，并按新 witness
事实订阅关系只重算受影响候选；deterministic/hybrid 使用保持当前位置同分
顺序的惰性堆，random 保持 softmax 分布。HDC/Jev 仍走完整评分路径，不复用
不满足其更新语义的缓存。R/W 都有 id→位置索引；delta 按 slot→plan 索引，
partial W 按 ground/open-side WaitKey 索引，不再逐边全表扫描。
`--fixpoint-star-order goal` 是独立的规则级调度消融，默认 input 保留目录序。

2026-11-06 Random 调度优化：Fixpoint 的 exact/null 增量路径使用 Gumbel-max，
为每个存活 action 保留一个独立 Gumbel 扰动，并以 `total/tau + gumbel` 复用
同一惰性堆。选最大扰动优先级与 softmax(total/tau) 抽样分布等价，但不再为
每次选择扫描整个 R；action 删除时释放扰动。固定 seed 在当前实现可复现，
不保证与旧的逐次全表 softmax 实现产生相同抽样轨迹。HDC/Jev 完整评分路径不变。

**递归 goal 的 pin 约束**：若 goal 谓词位于递归 SCC（含自循环），其
goal-headed 规则不再把全部完整匹配钉到最终常量，否则会同名中间事实被排除，
造成无 cap 的漏推（真实反例 p04；最小复现为单规则 gar_000798）。非递归
goal 仍固定结论两端。此修正同时作用于 input/goal 两种 star 顺序。

终止状态：goal / fixpoint / budget / timeout / candidate_cap（任何枚举触顶
--max-candidates-per-rule 即记，不声称完备）。输出：`fixpoint_trace.tsv`
（select/fair_select/evaluate/suspend/revive/prune/derive 事件）+ summary.yaml
的 fx_* 计数与 prepare/generation/selection/score/removal/evaluation/derivation/
delta/revival 分项耗时；delta 再细分 complete、partial-complete、partial-grow。
其中 `fx_match_embeddings` 是实际物化且通过顶点 label 的完整匹配（不含 matcher
内部回溯和未完成 partial），`fx_ready_insertions` 是 R 入队次数（含 revival），
`fx_evaluate_events` 是实际求值 action 数；三者均不等于最终派生数。
**注意**：action 级 learned policy（rollout 训练）**未实现**——现有 BNB
ValueModel 是分支级模型，不能冒充。

**证书语义**：bnb `--top-k 0`（默认）为精确模式——搜索穷尽时
`certified_optimal=true`（Theorem 5）或 `certified_no_chain=true`（预算不足
证书）+ anytime 贪心续跑回退（line 14）。证书覆盖"所见候选空间内的最优链"：
`--max-candidates-per-rule` 的 motif 生成截断是工程参数，不在证书范围内。

**已知局限（重要）**：
- greedy 无回溯，rel_fam 高的浅层 decoy 规则会吸干预算——fb15k olympics 区
  athletes↔medals_won 互逆规则对（gar_000064/63）即为例证：round-2 目标
  `athletes(/m/05r4w,/m/09n48)` 在 budget 30 与 400 下均 budget 耗尽
  （soundness 仍 pass）。
- 精确 bnb 的兄弟分支退役粒度是"每展开一个 motif"，等价 price 的候选在大
  DG 邻域里成百上千：fb15k 实测 ~0.1s 与 ~40MB/分支，budget 30 下 cap 256/
  cap 64/top-k 1 三种配置都在数分钟内展开上千分支而未完成。**对策**：
  `--top-k` 截断、`--max-candidates-per-rule` 限候选、紧凑 budget（line-4 的
  c+price≤B 剪枝随之生效）。正例：上述 goal 以 `bnb --budget 2` 找到最优
  2 步链（gar_000815 → gar_000063，certified_optimal，2084 展开/195s/12GB）。

### 3.3 HDC 价值模型：bnb 的学习型排序（workflow）

原理与机制详见 `docs/hdc_value_model.md`。三步工作流：

```bash
# 1) 采集训练轨迹（日志即教材，无需人工标注）
GAR_DEBUG_BNB=1 ./build/bin/gar_reason --mode agent --executor bnb \
    --csr <csr> --rules <rules> --goal 'rel(s,d)' --budget 2 \
    --max-candidates-per-rule 64 --out <dir> > goal.log 2>&1

# 2) 训练并导出模型（LOGO 防泄漏；脚本内需按实际情况配置日志清单）
python3 experiments/hdc_online/train_export.py   # 需 numpy；产出 meta.txt + weights.bin

# 3) 在线使用（只改展开顺序，bound/剪枝/证书/派生校验不变；默认关闭）
./build/bin/gar_reason --mode agent --executor bnb \
    --bnb-priority hdc-value --value-model <meta.txt> \
    --csr <csr> --rules <rules> --goal 'rel(s,d)' --budget 2 ...
```

在线 A/B（fb15k，4 个非平凡 goal，LOGO）：展开数 −87.2%、耗时 −86.4%、
最优成本/证书/chain.tsv 全同；synth 小图机制正确但无区分度。
出处：`experiments/hdc_online/`。

## 4. 分析工具链（gar_analyze）

子命令式单一二进制；逻辑全部在 analysis / engine 库，app 只做组合与 IO。

```bash
./build/bin/gar_analyze rules-stats  --rules <root> [--dot out.dot] [--yaml out.yaml]
./build/bin/gar_analyze gen-goals    --csr <dir> --rules <root> --per-slot N --seed S --out goals.tsv [--threads N]
./build/bin/gar_analyze gen-workload --csr <dir> --rules <root> --num-tasks N --seed S --out tasks.tsv [--budgets 0.5,1,2,4]
./build/bin/gar_analyze chain-cmp    --agent-out <dir> --closure-out <dir> [--goal 'rel(src,dst)']
./build/bin/gar_analyze verify       --run --csr <dir> --rules <root> --goal 'rel(src,dst)' [--budget B] [--policy ..] [--scorer ..] [--executor greedy|bnb] [--gpu]
./build/bin/gar_analyze verify       --from-dirs <agent> <cpu> [<gpu>] --goal 'rel(src,dst)'
./build/bin/gar_analyze verify       --tasks tasks.tsv --csr <dir> --rules <root> [--policy ..] ...
```

- **rules-stats**：规则集统计 + 依赖形态。实测 fb15k：209 谓词 / 3023 弧 /
  147 SCC（96 递归族，最大 28）/ 汇最深距离 10；Build 1.4ms。
- **gen-goals**：论文 §7 workload 构造——采样边**整体留出**（hold-out，
  `TripleGraph::RemoveEdgesBatch`）后跑一次无约束 chase，只保留闭包重新导出
  的目标。`bstar_round`=chase 首次导出轮次（经验口径），`bstar_dg`=DG 谓词级
  导出深度（下界口径，`DependencyGraph::DerivationDepths`）。实测：synth
  per-slot 4 → 7/7 可达；fb15k per-slot 1 → 110/167 可达（57 条因 10M 闭包
  上限被丢弃，stderr 有 NOTE）。
- **gen-workload**：每目标展开预算梯度 {0.5,1,2,4}×max(bstar_dg,1) 向上取整
  （紧预算锚定，天然适配 5b 的精确 bnb 规模发现）。实测 synth --num-tasks 4 →
  3 个可行目标 × 4 档 = 12 任务（小图整体留出干扰所致，见注）。
- **chain-cmp**：agent 输出目录 vs 闭包目录离线对账（子集断言 + 成本对比 +
  goal 行双向确认），子集违规退出码 1。实测 5a agent vs synth 基线：
  7 ⊆ 9 PASS，cost 7 vs 9。
- **verify**：三方对账。判定四值：CONSISTENT / BUDGET-LIMITED（goal 可达但
  预算不足，退出码 0）/ DIVERGENT（子集违规、足预算未达成的 fair 性违反、
  cpu/gpu 矛盾；退出码 1）/ INFEASIBLE（闭包外目标）。--run/--tasks 对全
  grounding 目标先 hold-out 目标边再跑 agent 与闭包（Γ0 = G \ {g}，论文
  task 语义）。实测 synth 批量 12 任务全 CONSISTENT，成功率 12/12。
  --tasks 是逐任务闭包，开销随任务数线性（fb15k 规模建议抽查）。

## 5. 规则检验（validate_rules）

挖掘 app 产规则，检验 app 验规则：在 KG 上重放每条规则
（`PatternMatcher` 同态 + 顶点 label 后过滤 + `graph::EvalAntecedent`
求 X），按挖掘器口径重算 **pivot-pair** 质量——support = 满足
body∧X∧head 的不同 pivot 对数，confidence = support / 满足 body∧X 的
不同 pivot 对数——与 rule.yaml 里的挖掘期存储值对账。

```bash
./build/bin/validate_rules \
    --csr /ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr \
    --rules /ssd_data/zhuxk/graphs/KG/fb15k-237/rules \
    --out /tmp/fb_validate.tsv --print 20
```

判定：PASS（support 相同且 |Δconf| ≤ `--conf-tol`，默认 1e-9）/ DRIFT
（重算值不同——图变了或计数有 bug，非零退出码，可做 CI 检查）/ SKIP
（ml 谓词、通配 label、纯查询规则）。`--out` 输出逐规则 TSV（双份
support/conf、body、embeddings、x_ok、capped、note）。

实证：synth 2/2、dbpedia rules_enriched（166 条带 X 规则）166/166、
fb15k 全规则集 1,652/1,652 全 PASS（同一图上挖掘值与重算值完全一致）；
人为篡改 support 后正确报 DRIFT。

## 6. 现有规则集

| 路径 | 规则数 | 说明 |
|---|---|---|
| `fb15k-237/rules` | 1,652 | 链式 k≤2（supp50/conf0.3） |
| `fb15k-237/rules_g3` | 81,683 | 通用拓扑 k≤3（supp50/conf0.3） |
| `ogbl-biokg/rules` | 56,601 | 类型化链式（supp200/conf0.3） |
| `finreflectkg/rules` | 455 | 链式（supp500/conf0.3） |
| `dbpedia/rules` | 607 | 链式（supp500/conf0.3） |
| `dbpedia/rules_enriched` | 166 | 带 X 谓词（supp500/conf0.3） |

根路径均为 `/ssd_data/zhuxk/graphs/KG/`。

## 7. 神经链接预测 baseline（NBFNet / A*Net）

实现位于 `baselines/path_reasoning/`，只依赖现有 PyTorch，不需要 TorchDrug、
torch-scatter 或 PyG。输入目录包含标准 `train.txt`、`valid.txt`、`test.txt`，每行
`head<TAB>relation<TAB>tail`。模型使用训练边和逆关系，支持负采样 BCE、验证集选模、
filtered MR/MRR/Hits@K。它们输出链接预测分数，不是精确 GAR 派生，不能与 Chase 的
逻辑闭包或 goal recall 混为同一指标。

```bash
python3 -m baselines.path_reasoning.cli train \
  --model nbfnet \
  --data /ssd_data/zhuxk/graphs/KG/fb15k-237/Release \
  --out .work_hdcv/nbfnet_fb15k --device auto \
  --epochs 20 --batch-size 4 --eval-batch-size 4

python3 -m baselines.path_reasoning.cli train \
  --model astarnet \
  --data /ssd_data/zhuxk/graphs/KG/fb15k-237/Release \
  --out .work_hdcv/astarnet_fb15k --device auto \
  --epochs 20 --batch-size 4 --eval-batch-size 4 \
  --node-ratio 0.1 --degree-ratio 1.0
```

论文默认结构为 6 层、32 维、DistMult 消息和 PNA 聚合。默认 batch=4 是当前原生
PyTorch 实现的资源设置，不是论文的官方融合内核性能配置。加载检查点评估：

```bash
python3 -m baselines.path_reasoning.cli eval \
  --checkpoint .work_hdcv/nbfnet_fb15k/best.pt \
  --split test --device auto --eval-batch-size 4
```

开发验证可使用 `--limit-train N --limit-eval N`；正式实验不得保留这两个限制。

## 8. GAR 链接预测管线

正式对照使用标准 FB15K-237 划分和完整 test split。规则挖掘、规则派生和神经模型
训练都只能读取 train；valid 只用于调参与选模，test 只用于最终评估。现有从
train+valid 图挖出的 rules640 不属于这个协议。数据冻结与 train-only CSR 构建：

```bash
python3 experiments/link_prediction/prepare_fb15k.py
```

默认输出 `.work_hdcv/fb15k_link_prediction/`，其中 `manifest.json` 保存三分割哈希、
计数和使用约束。挖掘与完整派生：

```bash
./build/bin/mine_rules \
  --csr .work_hdcv/fb15k_link_prediction/immutable_csr_train \
  --out .work_hdcv/fb15k_link_prediction/rules_train \
  --min-supp 50 --min-conf 0.3 \
  --min-body-len 1 --max-body-len 2 --print 0

./build/bin/gar_reason --mode full --engine cpu \
  --csr .work_hdcv/fb15k_link_prediction/immutable_csr_train \
  --rules .work_hdcv/fb15k_link_prediction/rules_train \
  --out .work_hdcv/fb15k_link_prediction/chase_train \
  --threads 4 --max-rounds 1000 \
  --max-derivations 1000000000 --max-matches 1000000000 \
  --no-matches --print 0
```

将完整闭包转为候选排名（`first-proof-confidence` 是首个可用评分，不是多证明最优
评分）：

```bash
python3 experiments/link_prediction/evaluate_gar_closure.py \
  --data .work_hdcv/fb15k_link_prediction/data \
  --rules .work_hdcv/fb15k_link_prediction/rules_train \
  --closure .work_hdcv/fb15k_link_prediction/chase_train/closure_edges.tsv \
  --out .work_hdcv/fb15k_link_prediction/gar_prediction
```

输出 `ranking.tsv` 与 `summary.json`，指标为 filtered MR/MRR/Hits@1/3/10。评估器拒绝
非 fixpoint 或 capped 的部分闭包，因为部分闭包不能用于完整候选排名。该管线与
NBFNet/A*Net 不共享模型代码，只共享标准数据划分和指标定义。

## 9. 规则蒸馏数据生成

此实验研究神经学生模仿 GAR 教师，不把教师派生标签称为独立的现实 ground truth。
生成器读取可观察图和完整教师闭包，输出 `observed.txt`、按 `(head,relation)` 查询组
隔离的 `train/valid/test.txt` 以及带教师规则的 `provenance.tsv`：

```bash
python3 experiments/rule_distillation/generate_dataset.py \
  --observed .work_hdcv/fb15k_link_prediction/data/train.txt \
  --closure .work_hdcv/fb15k_link_prediction/chase_train/closure_edges.tsv \
  --out .work_hdcv/fb15k_rule_distillation --seed 42
```

`manifest.json` 明确记录标签语义、拆分、输入哈希和 `development_limit`。学生模型读取
`observed.txt` 作为消息传递图，只把对应 split 的教师派生事实作为训练/评估标签：

```bash
python3 -m baselines.path_reasoning.cli train \
  --model nbfnet --data .work_hdcv/fb15k_rule_distillation \
  --out .work_hdcv/fb15k_rule_distillation/nbfnet \
  --device auto --epochs 20 --batch-size 4 --eval-batch-size 4

python3 -m baselines.path_reasoning.cli train \
  --model astarnet --data .work_hdcv/fb15k_rule_distillation \
  --out .work_hdcv/fb15k_rule_distillation/astarnet \
  --device auto --epochs 20 --batch-size 4 --eval-batch-size 4 \
  --node-ratio 0.1 --degree-ratio 1.0
```

评估指标解释为学生对教师答案的 agreement / imitation MRR、Hits@K，不与标准
FB15K-237 真实测试集准确率混合报告。`--limit` 仅用于开发数据量截断，正式生成不使用。

## 10. 原图测试 + 规则教师正负训练数据

这是独立的数据协议：在原图上用 `rules/` 做 rule deduction，默认最多取 100 万条派生
正样本；按 1:1 随机破坏头或尾生成负样本，并排除原图及全部派生事实。每个数据集输出到
`/ssd_data/zhuxk/graphs/KG/<dataset>/train/`：

- `train.txt`：`head<TAB>relation<TAB>tail<TAB>label`，label=1 为派生正例、0 为随机负例；
- `test.txt`：原图全部三元组，三列无标签；
- `positives.txt` / `negatives.txt`：三列正负样本；
- `provenance.tsv`：正例的派生轮次与教师规则；
- `rules`：指向该数据集原始规则目录的只读复用链接；
- `manifest.json`：输入哈希、样本数、seed、语义与开发截断记录。

串行完成 FB15K-237、ogbl-biokg、FinReflectKG 和 DBpedia：

```bash
python3 experiments/rule_distillation/run_kg_train.py --phase all
```

也可指定单个数据集：

```bash
python3 experiments/rule_distillation/run_kg_train.py \
  --datasets fb15k-237 --phase all --seed 42 --negative-ratio 1
```

每数据集外部上限 1000 秒、进程树及整机已用内存上限 256GiB。派生在自然 fixpoint
或 `--max-positive`（默认 100 万）处停止；后者在 manifest 中明确记录
`data_status=capped_partial_closure` 和 `closure_complete=false`，不得写成完整闭包。

NBFNet/A*Net 可直接读取该目录：第四列存在时使用已有 0/1 标签做 BCE，只用 label=1
的三元组构建消息传递图；`test.txt` 的全图三元组只作为评估标签，不作为输入图。没有
`valid.txt` 时跳过验证并使用 `last.pt`，不会根据 test 选模型。

```bash
python3 -m baselines.path_reasoning.cli train \
  --model nbfnet --data /ssd_data/zhuxk/graphs/KG/fb15k-237/train \
  --out .work_hdcv/fb15k_generated/nbfnet --device auto \
  --epochs 20 --batch-size 4 --eval-batch-size 4

python3 -m baselines.path_reasoning.cli train \
  --model astarnet --data /ssd_data/zhuxk/graphs/KG/fb15k-237/train \
  --out .work_hdcv/fb15k_generated/astarnet --device auto \
  --epochs 20 --batch-size 4 --eval-batch-size 4 \
  --node-ratio 0.1 --degree-ratio 1.0
```

用已有检查点对完整原图测试集评估（不要加 `--limit-eval`）：

```bash
python3 -m baselines.path_reasoning.cli eval \
  --checkpoint .work_hdcv/fb15k_generated/nbfnet/last.pt \
  --data /ssd_data/zhuxk/graphs/KG/fb15k-237/train \
  --split test --device auto --eval-batch-size 4
```
