# synth 小图上的 HDC 价值模型在线 A/B

日期：2026-10-28。资产：/tmp 被系统清理后按 README 重建
（mine_rules → /tmp/synth_rules，kg_loader convert → /tmp/synth_csr；
2 条规则，全闭包 4 条 round-1 + 5 条 round-2 派生边）。

## 数据与训练

- 9 个 goal：5 个 round-2（lives_in(p0/p3/p6,t1)、lives_in(p2/p5,t0)，
  链长 2）+ 4 个 round-1（链长 1）。配置 budget=5、cap=64、hybrid/exact、
  bnb + GAR_DEBUG_BNB。
- LOGO 防泄漏：每个 goal 的模型用其余 8 个 goal 的轨迹训练；训练节点
  33-42 个/模型（on-path 正例 12-13 个）——样本量如实记录，很小。

## A/B（utility → hdc-value）

| goal | 展开数 | Δ | best_cost | certified | chain.tsv |
|---|---|---|---|---|---|
| g0 (r2) | 8 → 10 | +25% | 2=2 ✓ | 1=1 ✓ | 一致 |
| g1 (r2) | 16 → 14 | -12.5% | 2=2 ✓ | 1=1 ✓ | 一致 |
| g2 (r2) | 10 → 12 | +20% | 2=2 ✓ | 1=1 ✓ | 一致 |
| g3 (r2) | 18 → 16 | -11.1% | 2=2 ✓ | 1=1 ✓ | 一致 |
| g4 (r2) | 12 → 12 | 0% | 2=2 ✓ | 1=1 ✓ | 一致 |
| g5-g8 (r1) | 1/3/1/3 → 同 | 0% | 1=1 ✓ | 1=1 ✓ | 一致 |
| 合计 | 70 → 65 | -7.1% | — | — | — |

模型开销：12-13µs/次推理，每 run ≤0.3ms（synth 单 run 仅数 ms），可忽略。

## 正确性

- best_cost / certified_optimal 全部一致；chain.tsv 9/9 一致；
- soundness: pass（lives_in(p0,t1)，--verify-against-closure）；
- 默认关闭：chain.tsv + summary.yaml 逐字节一致。

## 判定

**小图无区分度，机制正确但收益无法体现**（与预期管理一致，未硬造数字）：
round-1 goal 全平凡（父分支首展开即解出，A/B 恒等）；round-2 goal 展开数
8-18 的尺度下，模型排序在 ±25% 噪声内抖动（2 胜 2 负 1 平），合计 -7%。
每个 LOGO 模型仅 ~35 个训练节点，预测无统计意义。机制（ValueModel 加载、
hdc-value 优先级、正确性保证、默认关闭回归）在小图上全部工作正常；
价值信号仍需 fb15k 量级的非平凡 goal（参照主 A/B：r2c_2/r2c_4 各 -93%）。
