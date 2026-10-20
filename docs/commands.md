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

输出（`<out>/`）：`summary.yaml`（逐规则 embeddings/x_satisfied/derived +
总轮数/不动点标记）、`closure_edges.tsv`（新事实：src、relation、dst、推导轮次、
来源规则）、`matches/<rule>.tsv`（末轮全部匹配 + x_ok/head 标记）。

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

单链朝目标事实演绎（greedy）或带证书的最优链搜索（bnb，Figure 6 HybNS）：

```bash
./build/bin/gar_reason --mode agent --csr <immutable_csr_dir> --rules <rules_root> \
    --goal 'rel_name(src,dst)' --budget 30 \
    [--policy deterministic|random|hybrid] [--scorer exact|hdc] [--hdc-dim 8192] \
    [--executor greedy|bnb] [--top-k N] [--seed 42] [--max-candidates-per-rule 256] \
    [--alpha 1] [--beta 1] [--gamma 0.5] [--nu 1] [--nu2 0.5] [--tau-hdc 0.15] \
    [--no-confidence] [--out <dir>] [--verify-against-closure] [--print 20]
```

| 参数 | 默认 | 说明 |
|---|---|---|
| `--goal 'rel(s,d)'` | 必填 | 目标事实；实体名，`_` = 开放变元 |
| `--budget N` | 100 | 单链成本上限（unit cost：一次应用 = 1，成败都计费） |
| `--policy` | deterministic | argmax / softmax(total/τ)（`--seed` 可复现）/ admissible 硬过滤+argmax |
| `--scorer` | exact | exact（见证前提比例）/ hdc（cos(v_μ,E) 证据向量读数） |
| `--executor` | greedy | greedy（§5.1 提交式）/ bnb（Figure 6 分支定界） |
| `--top-k N` | 0 | bnb 每分支候选截断；0=精确模式（带证书） |
| `--hdc-dim N` | 8192 | HDC 维度（档位 2 的幂，128…16384） |
| `--max-candidates-per-rule` | 256 | 每规则每步 motif 枚举上限 |
| `--alpha/--beta/--gamma/--nu/--nu2/--tau-hdc` | 1/1/0.5/1/0.5/0.15 | 打分权重（total=α·(rel_fam+ν·rel_evd+ν2·conf)−β·dcost） |
| `--verify-against-closure` | 关 | app 层另跑 CpuChaseEngine 全闭包，断言 agent 导出 ⊆ 闭包（soundness 写入 summary.yaml） |

输出（`--out`）：`chain.tsv`（每步 rule/embedding/成败/成本/导出事实）、
`closure_edges.tsv`（与 full 同布局）、`summary.yaml`（goal/derived/成本/
终止原因/达成 + bnb 的 certified_optimal/certified_no_chain/anytime_fallback/
utility/expansions）。

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
