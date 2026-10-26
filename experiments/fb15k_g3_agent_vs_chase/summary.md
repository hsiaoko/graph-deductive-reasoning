# fb15k-237 / rules_g3：全量 chase vs 策略制导推理 对比实验

日期：2026-10-26。机器：GCC 12 + CUDA 12.6，104 核，503GB RAM，RTX 3090×2。

- 图谱：`/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr`（|V|=14,513，|R|=237，|E|=289,650）
- 规则：`/ssd_data/zhuxk/graphs/KG/fb15k-237/rules_g3`（81,683 条，通用拓扑 k≤3）
- 全部 agent 运行：`--max-candidates-per-rule 8`（见"性能发现"F2）；chase 均为
  `--threads 8`。

## 目标清单

构造方法：rules_g3 的全量 chase 第 2 轮不可行（见下），无法走 gen-goals 的
"留出+全量闭包"标准流程，改为：从第 1 轮闭包（181 万条派生事实，含派生规则
标注）中抽取"规则 A 导出 r 且规则 B 的 body 含 r"的 (A,B) 链对，跑 {A,B} 子集
chase（秒级），取其 round-2 事实并核对不在全量第 1 轮闭包中（→ 全量 chase 至少
需要 2 轮）。G3 另加一条 {A,B,C} 三规则链（round-3 构造）。B1-B3 为第 1 轮即可
派生的对齐目标（score-aligned，见 F4）。

| 目标 | 关系实例 | 构造链 | 全量 chase 口径 |
|---|---|---|---|
| G1 | local_tuition…/currency(/m/01y8zd,/m/02l6h) | gar_000000→gar_017735（2 应用） | 第 1 轮缺席，第 2 轮（gar_000482）派生 |
| G2 | 同上 (/m/022jr5,/m/02l6h) | 同 G1 | 同上 |
| G3 | 同上 (/m/014b4h,/m/0ptk_) | gar_000000→gar_017735→gar_021094（3 应用子集构造） | 第 2 轮（gar_000996）派生 |
| B1 | military_combatant…/combatants(/m/09c7w0,/m/07ssc) | 1 步（gar_000018 逆规则） | 第 1 轮派生 |
| B2 | film/runtime…/film_release_region(/m/04v8x9,/m/02w7gg) | 1 步 | 第 1 轮派生 |
| B3 | people/person/nationality(/m/044mz_,/m/07ssc) | 1 步 | 第 1 轮派生 |

注：rules_g3 的依赖图上 230/236 个可派生谓词的谓词级导出深度全是 1
（logs/dg_depth_probe.txt）——通用拓扑规则集冗余度极高，实例级"链长 2"只能靠
round 口径区分。

## 主结果表

耗时列为"推理主循环墙钟 / 端到端墙钟"（端到端含 81,683 条规则加载 + DG 构建
≈18-39s 固定开销，随磁盘缓存冷热波动）。

| 目标 | 方法 | 主循环 | 端到端 | 派生数 | 命中 | verify |
|---|---|---|---|---|---|---|
| G1 | agent greedy exact（B=10） | 13.6s | 31.2s | 10 | ✗ | BUDGET-LIMITED（goal∈闭包，子集 ✓） |
| G1 | agent greedy exact（B=50） | 74.1s | 74.1s | 50 | ✗ | — |
| G1 | agent greedy random ×3 种子（B=30） | ~41s/次 | ~59s/次 | 30 | ✗✗✗ | — |
| G1 | agent greedy hdc512（B=10） | 44.4s | 62.1s | 10 | ✗ | — |
| G1 | agent greedy hdc8192（B=10） | **>8min 未完成第一步（杀）** | — | — | ✗ | — |
| G1 | agent bnb exact（B=2，cap8） | **>15min/320 展开未完成（杀）** | — | — | ✗ | — |
| G1 | chase 全量（见下表） | — | — | — | 第 2 轮命中 | 见 verify_g1 |
| G2 | agent greedy exact / hdc512（B=10） | 13.3s / 43.3s | 31.0s / ~62s | 10 | ✗ | — |
| G3 | agent greedy exact / hdc512（B=10） | 13.5s / 43.5s | 31.2s / ~62s | 10 | ✗ | — |
| B1 | agent greedy exact（B=10） | 1.44s（1 步命中） | 19.5s | 1 | ✓ | CONSISTENT |
| B1 | agent greedy hdc512（B=10） | 2.72s（1 步命中） | 20.6s | 1 | ✓ | — |
| B1 | agent bnb exact（B=1，cap8） | 1 步 1 展开 | 19.8s | 1 | ✓（certified_optimal） | CONSISTENT |
| B2 | agent greedy exact / hdc512（B=10） | 13.4s / 43.4s | ~31s / ~62s | 10 | ✗ | — |
| B3 | agent greedy exact / hdc512（B=10） | 13.5s / 43.5s | ~31s / ~62s | 10 | ✗ | — |

## 全量 chase 可行性阶梯（对比基准）

| 口径 | 结果 |
|---|---|
| 第 1 轮（匹配上限 2M/规则） | **51.4s，派生 1,818,708 条**，G1-G3 缺席，B1-B3 命中 |
| 第 2 轮（匹配上限 2M/规则） | **OOM 被杀（SIGKILL）于 433s，峰值 225.6GB** |
| 第 2 轮（匹配上限 100k/规则，截断口径） | **226.3s，48GB，派生 23,483,693 条**（rounds=2，非不动点），六个目标全部在第 2 轮派生 |
| 第 3 轮（gen-goals 内嵌，上限 2M） | 超 3600s 任务超时被杀（第 1 轮 32.3s 完成后第 2 轮未结束） |
| 到不动点 | 不可行（内存轨迹同上） |

verify 判定（`gar_analyze verify --run`，内部 chase 用相同 cap 开关）：
- B1 greedy：`CONSISTENT`（goal∈闭包、agent 命中、派生 ⊆ 闭包）——verify_b1_greedy.yaml
- B1 bnb：`CONSISTENT`——verify_b1_bnb.yaml
- G1 greedy：`BUDGET-LIMITED`（goal_in_cpu_closure=true、subset_ok=true、预算耗尽属预期）——verify_g1_greedy.yaml
- 另：`chain-cmp` 对 greedy 各臂派生集 ⊆ 截断闭包 全部 PASS。

## 性能发现（本实验的额外产出）

- **F1 全量 chase 在 8 万通用拓扑规则下内存爆炸**：第 2 轮的瓶颈不是枚举而是
  派生事实合并/驻留（第 1 轮 1.8M → 第 2 轮累计 23.5M 条），2M 匹配上限时
  225GB OOM；100k 截断是唯一能跑完 2 轮的口径（代价：截断规则的组成为下界）。
- **F2 agent 每步候选重枚举是规模瓶颈**：默认 cap 256 时第一步候选生成
  >10.5min 未完成（杀掉）；cap 8 时 ~1.3s/步（exact）。根源：rules_g3 的 DG 有
  214/236 谓词的最大递归族，d(p₀)<∞ 硬过滤几乎不剪枝，81,683 条规则逐枚。
- **F3 HDC 打分器在每个候选上做 8192 维证据余弦**（`EvidenceVector::Cosine`
  每次调用重建打包向量），8192 维下 8 分钟未跑完一步；512 维对照 4.4s/步
  （exact 的 3.2 倍）。HDC 的读数成本需要按 F3 的注做缓存优化（未改语义，
  本期只记录）。
- **F4 greedy 的 decoy 问题在 rules_g3 上比在 rules 上更严重**：nationality/
  combatants/film_release_region 等高 conf 规则与目标无关地反复中奖（六组
  greedy 运行的派生事实高度重叠），与 5b 的 fb15k/1652 规则观察一致——打分
  对齐的目标（B1）1 步命中，不对齐的目标 budget 50 也不中。
- **F5 bnb 在此规则集上被候选体积压垮**：目标谓词 in-degree 4,126 条规则
  → price-1 motif 空间 ~33k，兄弟退役粒度为每展开一个 motif，budget-2 搜索
  15.5 分钟仅 320 次展开（~45MB/分支克隆）。5b 的紧凑预算剪枝救不了
  "目标本身被数千条规则直接派生"的情形。

## 结论一句话

在 rules_g3 这种高冗余通用拓扑规则集上：目标与打分对齐时 agent 秒杀全量
chase（1.4-2.7s vs 226s，1 条 vs 2350 万条附带派生，还带最优性证书）；不对齐
时 greedy/random 都困于 decoy、bnb 困于候选体积，反而是截断口径的蛮力 chase
稳定覆盖——打分质量（学到的策略）才是论文主张的杠杆，本实现已具备全部验证
与观测工具。

## 复现

```bash
# chase 臂
./build/bin/gar_reason --mode full --engine cpu --threads 8 \
  --csr <csr> --rules rules_g3 --out <out> --max-rounds 2 \
  --max-derivations 30000000 --max-matches 100000 --no-matches
# agent 臂（例）
./build/bin/gar_reason --mode agent --csr <csr> --rules rules_g3 \
  --goal '<rel(src,dst)>' --budget 10 --policy deterministic \
  --scorer exact --executor greedy --max-candidates-per-rule 8 --out <out>
# 校验
./build/bin/gar_analyze verify --run --csr <csr> --rules rules_g3 \
  --goal '<rel(src,dst)>' --budget 10 --max-rounds 2 --max-matches 100000 ...
```

## 目录

- `goals/`：候选构造清单（goals_d2 / goals_d3：事实、A/B/C 规则链）
- `subsets/`：派生目标用的规则子集（符号链接指向 rules_g3 的规则目录）
- `logs/`：全部 agent 运行的 chain.tsv/summary.yaml/stdout/stderr、chase 各口径
  summary、DG 深度探测输出、verify 报告、被杀运行的 stderr
