# 消融实验：HDC 打分与依赖图制导对 goal-directed 推理的贡献

日期：2026-10-26。图谱 fb15k-237（|V|=14,513，|E|=289,650），规则集
rules_g3（81,683 条通用拓扑规则）。固定参数：executor=greedy、budget=10、
--max-candidates-per-rule 8、--threads 8、seed=42（E 组 1/2/3）、
hdc_dim=512（8192 维已在上轮证实不可行：8 分钟未跑完一步，见 F3）。

**harness**：`gar_analyze verify --tasks --shared-holdout`（论文 §7 原义：全部
任务目标整体留出 Γ0=G\X，一次共享闭包；本期的工程新增，见下）。chase 口径：
2 轮 + 100k 匹配截断 + 30M 派生上限（上轮证实 2M 上限 OOM@225GB）。

## 工程改动（各自独立小 commit，默认行为不变）

- `befb6e7` `--no-dg`：ReasoningAgent 加 guide_with_dg 构造参数；关闭时 d(·)
  表全零（候选硬过滤不剪枝、rel_fam=γ⁰=1 中性、price=w₀）；DG 仍构建
  （失败分类的 static/dynamic 判定读它的 in-degree）。synth 回归逐字节一致。
- `445c975` verify --tasks 共享闭包快路径（hold-out 为空操作时全任务复用一次
  闭包）+ 报表加 agent_wall_ms 列。synth 回归 12/12 不变。
- `ee3a67c` verify --tasks --shared-holdout：逐任务留出（默认，phase-6 行为）
  与整体留出（论文原文口径）双模式。
- （前序）`baa193b`/`d0ccfa8`/`9a3bef6`：gen-goals/gen-workload/verify 的
  chase cap 开关（--max-rounds/--max-matches/--max-derivations/--threads）。

## 任务集

`tasks.tsv`：gen-workload --num-tasks 20 --seed 42（budgets 列按任务要求统一改
为 10；bstar_dg 全为 1——rules_g3 的 230 个可派生谓词谓词级深度全是 1，见
logs/dg_depth_probe.txt（上轮）；bstar_round：18×1 + 2×2）。任务原始生成记录
tasks_raw.tsv。所有目标均可达（闭包重派生）。

## 主结果（20 任务 × 6 配置）

| 配置 | policy | scorer | DG | 命中率 | 平均主循环耗时/任务 | 平均步数 | 判定分布 |
|---|---|---|---|---|---|---|---|
| A 全开 | deterministic | hdc512 | ✓ | 0/20 | 43.6s | 10.0 | 20 BUDGET-LIMITED |
| B w/o HDC | deterministic | exact | ✓ | 0/20 | 13.5s | 10.0 | 20 BUDGET-LIMITED |
| C w/o DG | deterministic | hdc512 | ✗ | 0/20 | 43.8s | 10.0 | 20 BUDGET-LIMITED |
| D w/o 两者 | deterministic | exact | ✗ | 0/20 | 13.6s | 10.0 | 20 BUDGET-LIMITED |
| E 随机 | random ×3 种子 | exact | ✓ | 0/20（三个种子均） | 13.7s | 10.0 | 20 BUDGET-LIMITED ×3 |
| F 全量 chase | — | — | 227s（2 轮截断）/任务无关 | 20/20 可达（构造保证） | — | ground truth |

所有任务所有配置均无 DIVERGENT（子集校验全部通过，soundness 保持）。
端到端墙钟（含 39s 规则加载/DG + 227s 共享闭包，每配置一次）：
A 1089s / B 485s / C 1100s / D 492s / E 497s±2（3 种子）。

**地板效应**：均匀采样的可达目标上 greedy 全军覆没——81,683 条规则下每步
~65 万候选中 argmax 命中有信号目标的概率极低（上一轮 F4 的 decoy 观察）。
这个 0 命中率基线本身回答不了"制导有没有用"，所以加了对照探针：

## 对照探针：打分对齐目标上的消融（上一轮的 B1，已知 1 步可达）

目标：`combatants(/m/09c7w0,/m/07ssc)`（1 步可派生，上一轮 greedy/bnb 均命中）。

| 配置 | 命中 | 步数 |
|---|---|---|
| A（hdc+DG） | ✓ | 1 |
| B（exact+DG） | ✓ | 1 |
| C（hdc，无 DG） | ✗ | 10（预算尽） |
| D（exact，无 DG） | ✗ | 10（预算尽） |
| E（random ×3） | ✗✗✗ | 10 |

（原始输出在 logs/aligned_*/。）

## 三个问题的答案

**HDC 相对 exact：无效果（命中率），负效果（耗时）。**
两个工作负载上命中模式完全一致（主矩阵 0/20 vs 0/20；探针目标双双 1 步命中）。
链级轨迹从第 2 步起分叉（rel_evd 读数不同导致排序不同——上一轮的 chain.tsv
逐字节对比证实），但结局不变。代价：每步 ~3.2× 慢（43.6s vs 13.5s）；8192 维
则直接不可用（每候选都要重建证据打包向量 + 逐前提余弦，8 分钟跑不完一步）。
注：未命中时两者都跑满 budget=10 步，时间差 = 纯打分开销。

**DG 制导：主矩阵无效果，探针上有决定性正效果。**
主矩阵上 C/D 与 A/B 全同（0/20、同 10 步、同耗时）；抽查单任务开/关 DG
的链**逐字节一致**（logs/dg_toggle/）——rules_g3 的 DG 有 214/236 谓词的最大
递归族，d(p₀)<∞ 硬过滤几乎不剪枝，而 conf=1 的 decoy 规则在 rel_fam 加不加权
时都霸榜，制导信号被冗余淹没。但在打分对齐的探针目标上，DG 的 rel_fam 排序是
命中与否的唯一差别（B 中 D 不中）——制导在"信号存在时"有效。
结论：DG 制导**有真实但条件性的正效果**；在均匀随机目标 + 高冗余规则集上
观测不到（无效果）。

**随机策略比 deterministic 差：在主矩阵上持平（都 0/20），在探针目标上更差
（0/3 种子命中 vs deterministic 1/1）。** 随机探索没有帮它跳出 decoy 区；
耗时与 deterministic 相同（497s vs 485s 端到端）。

## 复现

```bash
# 任务集
./build/bin/gar_analyze gen-workload --csr <csr> --rules rules_g3 \
  --num-tasks 20 --seed 42 --budgets 1 --threads 8 \
  --max-rounds 2 --max-matches 100000 --max-derivations 30000000 --out tasks_raw.tsv
# （budget 列统一改写为 10 得到 tasks.tsv）
# 每配置一次（A 为例；B/C/D/E 换 --scorer exact / 加 --no-dg / --policy random --seed S）
./build/bin/gar_analyze verify --tasks tasks.tsv --csr <csr> --rules rules_g3 \
  --executor greedy --max-candidates-per-rule 8 \
  --policy deterministic --scorer hdc --hdc-dim 512 --seed 42 \
  --threads 8 --max-rounds 2 --max-matches 100000 --max-derivations 30000000 \
  --shared-holdout --report reports/A.tsv
```

## 目录

- `tasks_raw.tsv` / `tasks.tsv`：任务清单（后者 budget 列统一为 10）
- `reports/`：每配置的 verify 逐任务报表（goal/预算/判定/步数/成本/耗时）
- `logs/`：每配置的 stdout/stderr、aligned_*（探针运行完整输出）、
  dg_toggle/（DG 开关链对比，逐字节一致）
