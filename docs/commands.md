# 命令手册：挖掘与规则查看

GARAgent 挖掘管线的完整 CLI 参考。管线三阶段：

```
mine_rules（ChainMiner，链式）──┐
mine_graph_rules（PatternMiner，任意拓扑）┘→ match_pattern（GPU 回放）→ mine_predicates（X 谓词）
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

## 2.5. 演绎（deduce，逻辑闭包）

给定规则集 + KG，chase 到不动点：**拓扑（同态语义）+ X 谓词
（const/var/exists/edge 逐 embedding 求值）都生效**，推出的新边逐轮并回图索引
（含逆槽位），支持跨规则链式推导。双引擎，CLI 与输出布局完全相同：

```bash
# CPU（engine::deduction::CpuChaseEngine，轮内 oneTBB 规则级并行）
./build/bin/deduce \
    --csr /ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr \
    --rules /ssd_data/zhuxk/graphs/KG/fb15k-237/rules \
    --out /tmp/fb_closure \
    --max-rounds 100 --max-derivations 10000000 --max-matches 1000000 \
    --print 20

# GPU（MatrixGraph task::GARDeduce：匹配/X 求值/head 发射全在 device）
./build/bin/deduce_gpu \
    --csr /ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr \
    --rules /ssd_data/zhuxk/graphs/KG/fb15k-237/rules \
    --out /tmp/fb_closure_gpu \
    --max-rounds 100 --max-derivations 10000000 --max-matches 1000000 \
    --print 20
```

| 参数 | 默认 | 说明 |
|---|---|---|
| `--max-rounds N` | 100 | chase 轮数上限（head 不引新顶点，理论必终止） |
| `--max-derivations N` | 10M | 推出新事实总数上限 |
| `--max-matches N` | 1M | 单规则单轮 embedding 上限（超出记 capped，统计为下界） |
| `--no-matches` | 关 | 不写末轮匹配文件（省磁盘） |
| `--print N` | 20 | CLI 按 derived 降序打印前 N 条规则 |

输出（`<out>/`）：`summary.yaml`（逐规则 embeddings/x_satisfied/derived +
总轮数/不动点标记）、`closure_edges.tsv`（新事实：src、relation、dst、推导轮次、
来源规则）、`matches/<rule>.tsv`（末轮全部匹配 + x_ok/head 标记）。

实证：synth 3 轮推出 9 条（含跨轮推导）；dbpedia 带 X 规则验证谓词计数与挖掘期
口径一致（x_ok − derived = 挖掘 support）；fb15k 全 1,652 规则 10M 事实上限 4 轮
**17.6 秒**（轮内 oneTBB 规则级并行 + `AddEdgesBatch` 批量索引合并；输出与串行
逐字节一致）。GPU 版（`deduce_gpu`）：匹配/X 求值/head 发射在 device
（候选 cub 分段排序 + 二分搜索邻接段扫描 join，属性列 device 常驻），主端共享
CPU 的轮驱动/预过滤/合并/输出。双端对账：synth、dbpedia 2 轮、fb15k 2 轮无截断
（648,595 事实）均 `summary.yaml` 逐字节一致 + 闭包集合一致；有 match 截断时
仅截断规则内组成不同（子集选择顺序差异，预期行为）。耗时：fb15k 全闭包
GPU 49 秒 vs CPU 17.6 秒；dbpedia 2 轮 GPU 32 秒 vs CPU 21 秒。

## 2.6. 规则检验（validate_rules）

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

## 3. 现有规则集

| 路径 | 规则数 | 说明 |
|---|---|---|
| `fb15k-237/rules` | 1,652 | 链式 k≤2（supp50/conf0.3） |
| `fb15k-237/rules_g3` | 81,683 | 通用拓扑 k≤3（supp50/conf0.3） |
| `ogbl-biokg/rules` | 56,601 | 类型化链式（supp200/conf0.3） |
| `finreflectkg/rules` | 455 | 链式（supp500/conf0.3） |
| `dbpedia/rules` | 607 | 链式（supp500/conf0.3） |
| `dbpedia/rules_enriched` | 166 | 带 X 谓词（supp500/conf0.3） |

根路径均为 `/ssd_data/zhuxk/graphs/KG/`。

## 策略制导演绎（gar_reason --mode agent，论文 §3-§5）

单链 greedy 朝目标事实演绎；全闭包入口见 `--mode full`（cpu|gpu）。

```bash
./build/bin/gar_reason --mode agent --csr <immutable_csr_dir> --rules <rules_root> \
    --goal 'rel_name(src,dst)' --budget 30 \
    [--policy deterministic|random|hybrid] [--scorer exact|hdc] [--hdc-dim 8192] \
    [--executor greedy] [--seed 42] [--max-candidates-per-rule 256] \
    [--alpha 1] [--beta 1] [--gamma 0.5] [--nu 1] [--nu2 0.5] [--tau-hdc 0.15] \
    [--no-confidence] [--out <dir>] [--verify-against-closure] [--print 20]
```

- goal 参数为实体名，`_` 表示开放变元；关系名经图的关系字典解析为 slot。
- 输出（--out）：`chain.tsv`（每步 rule/embedding/成败/成本/导出事实）、
  `closure_edges.tsv`（与 deduce 同布局）、`summary.yaml`（goal/derived/成本/终止原因/达成）。
- `--verify-against-closure` 在 app 层另跑 CpuChaseEngine 全闭包并断言
  agent 导出 ⊆ 闭包（soundness 写入 summary.yaml）。
- 已知局限：greedy 无回溯，rel_fam 高的浅层 decoy 规则会吸干预算
  （fb15k olympics 区的 athletes↔medals_won 互逆规则对即为例证）；深层目标的
  最优链交给 BranchBoundExecutor（price 重排，下一阶段）。
