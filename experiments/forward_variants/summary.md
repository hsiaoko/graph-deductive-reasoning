# forward_variants：forward agent 变体矩阵

日期：2026-10-30。协议与预注册判据见 NOTES.md（含执行期协议修正 6 条）。

**设置**：fb15k-237，规则子集 1,652 条（/ssd_data/.../fb15k-237/rules）；
Γ0 = shared-holdout 基图（289,600 边，`.work_hdcv/csr_holdout`，全 50 个
task goal 边一次性移除，与论文 §7 及 fb15k_overall --shared-holdout
口径一致）。Goal 集 = 50 个 Overall goal 中**子集下可行的 22 个**
（per-task verify 判定 + shared-holdout 复核 22/22，判定明细
`.work_hdcv/feas_raw.tsv` / `feas_probe_res.tsv` /
`feas_shared_holdout.tsv`）。Ground truth = held-out 基图 × 子集的
capped chase 闭包（`.work_hdcv/fw_gt_closure_ho`，10M 截断，4 轮；
截断只影响 round-4 内部的子集选择，逐 goal 成员判定以 verify 重算
为准）。

## 主矩阵：greedy（budget=50, cap=64；22 可行 goal）

budget=50 是 greedy 在该 workload 上产生区分度的最低档（budget=10
cap=8/64 全配置 0/22 地板，见下"地板效应"）。

| 配置 | 命中 | wall mean/med/p90 (s) | 平均派生 | 命中 goal（steps） |
|---|---|---|---|---|
| deterministic + exact | **3/22** | 1.57 / 1.33 / 2.39 | 43.0 | g17(15) g19(25) g20(32) |
| deterministic + **null**（无打分） | 0/22 | 1.52 / 1.60 / 2.01 | 46.5 | — |
| deterministic + exact + **--no-dg** | 0/22 | 3.91 / 3.88 / 4.07 | 50.0 | — |
| deterministic + **hdc**(512) | 0/22 | 3.44 / 3.66 / 5.20 | 46.5 | — |
| random + exact（seed 42/7/13） | 0/22 ×3 | 1.74 / 1.82 / 2.40 | 46.5 | — |
| hybrid + exact | **3/22** | 1.57 / 1.33 / 2.39 | 43.0 | 同 det_exact 三 goal 同步数 |
| jev-mock + exact | 2/22 | 1.59 / 1.38 / 2.45 | 42.9 | g17(**4**) g19(**15**) |

明细：`.work_hdcv/fw_greedy_b50_detail.tsv`（每 run：hit/steps/cost/
derived/termination/wall）。

## bnb（budget=2, cap=64, top_k=0, 30min 超时）

**命中 goal（3 个最易 goal，两臂全量复现 Overall）**：

| goal | arm | hit | cost | expansions | certified | wall (s) | chain |
|---|---|---|---|---|---|---|---|
| g17 | utility | ✓ | 1 | 63 | ✓ | 5.78 | 两臂逐字节一致 |
| g17 | hdc-value | ✓ | 1 | 63 | ✓ | 5.75 | 同上 |
| g19 | utility | ✓ | 1 | 13 | ✓ | 1.29 | 同上 |
| g19 | hdc-value | ✓ | 1 | 13 | ✓ | 1.29 | 同上 |
| g20 | utility | ✓ | 1 | 33 | ✓ | 3.33 | 同上 |
| g20 | hdc-value | ✓ | 1 | 33 | ✓ | 3.38 | 同上 |

hdc-value 与 utility 在命中 goal 上展开数、链、耗时完全一致（小树、
值函数不改变排序）——与 Overall 结论一致：**bnb 处 HDC 值模型无净
效果**（既不增益也不减速）。

**难 goal（预算内穷举失败）**：g0 两臂各 941 展开 ~90s 未命中
（best_cost 未找到）。其余 18 个难 goal 未在本轮跑（每个最坏 30min
T/O，超出时间盒；Overall 同口径下这些 goal 两臂均为穷举失败或
30min T/O，无新增信息预期）。已完成部分明细见
`.work_hdcv/fw_bnb_detail.tsv` 与 `experiments/forward_variants/`。

**bnb + null（uniform-cost）臂中止**：g0 上 6,912+ 展开且 open=6,911
（open 随展开线性增长），~11 min RSS 297GB 仍 best_cost=-1，在看门狗
触发前主动中止（`.work_hdcv/fw_bnb/bnb_null/ABORTED.md`）。对照同
goal utility 臂 941 展开 89s——price 下界是 bnb 可控的决定因素，
这本身就是 Q① 在 bnb 上的答案。

## 地板效应（区分度边界）

| budget / cap | det_exact | det_null | det_hdc | random×3 | hybrid | jev-mock |
|---|---|---|---|---|---|---|
| 10 / 8 | 0/22 | 0/22 | 0/22 | 0/22 ×3 | 0/22 | 0/22 |
| 10 / 64 | 0/22 | 0/22 | 0/22 | 0/22 ×3 | 0/22 | 1/22 |
| **50 / 64** | **3/22** | 0/22 | 0/22 | 0/22 ×3 | **3/22** | 2/22 |

终止形态（cap=8）：每配置 20/22 budget 耗尽 + 2 no_candidates。

## 三个问题的答案

**① scoring 有无（exact vs null，同 policy=deterministic）**：决定性。
exact 3/22 vs null 0/22；null 下 greedy 退化候选枚举序，22 个可行
goal 全灭。耗时无差（1.57s vs 1.52s）——scoring 的成本可忽略，
收益是命中从 0 到 3。

**② policy（deterministic / random×3 / hybrid，同 scorer=exact）**：
deterministic 3/22 = hybrid 3/22（同三 goal、同步数——hybrid 在该
workload 上实际落在确定性选择上）>> random 0/22（3 seed 一致全灭，
方差 0）。jev-mock 2/22 但步数效率最高（g17 仅 4 步 vs det 15 步）。

**③ HDC 两处**：
- scorer=hdc(512) vs exact：**负效果**——0/22 vs 3/22，且 ~2.2× 慢
  （3.44s vs 1.57s mean）。与上轮消融结论（无命中增益、更慢）一致。
- bnb hdc-value vs utility：**无净效果**——3 个命中 goal 上展开数
  （63/13/33）、cost=1、certified、chain 逐字节一致、耗时一致。

**①（bnb 侧补充）**：scoring 对 bnb 是生存性问题——去掉 price 下界
（null scorer → uniform-cost）后，同一难 goal 上 open 前沿爆炸
（11 min 297GB 未决 vs utility 941 展开 89s 证毕），实验上无法跑完，
定性结论：utility 的 cost+price 优先级使 bnb 从不可行变为可行。

**机制（g17 剖析）**：goal 距基图 1 步（bstar=1，gar_001039 直接可
推），但 greedy exact/hdc 都被"/m/0b90_r 吸引子"困住——反复用同一
规则推 /m/0b90_r 的 religions 事实（前提见证度更高的 embedding 排
名靠前），goal embedding 排名低；exact 在 budget 50 时耗尽高排名候
选后（已派生=不可再选）于第 15 步命中，hdc 的排名 50 步内始终翻不
到 goal embedding。no-dg 把 rel_fam/price 的距离表拆掉后同样全灭且
最慢——DG 距离是 scoring 的主要信息量来源。

## soundness

- b50 三个命中 run 的全部派生边（15/25/32 条）逐条核对 ⊆ GT 闭包：
  零越界。
- 可行性 sweep（shared-holdout, budget=10 greedy）DIVERGENT=2 均为
  agent `no_candidates` 提前终止的覆盖缺口（g9 派生 0 条；g23 派生
  1 条已核对 ∈ 闭包 round 1）——非真实派生越界。
- `--scorer null` 最小开关单独 commit（44a74b7），默认 exact/hdc
  路径逐字节回归（synth bnb chain 一致）。

## g3 抽查（方向一致性）

rules_g3 81,683 全量 × det_exact / det_null / det_hdc × 5 goal（含三
个 bnb 命中 goal），budget=10 cap=8：**15/15 全灭**（budget 耗尽）——
与子集 cap=8 地板行、Overall greedy 0/49 方向完全一致：g3 上 greedy
无区分度，主矩阵放在子集是正确的。明细 `.work_hdcv/fw_g3_spot/`。

## 复现

```bash
# held-out CSR（一次性）：python3 过滤 triple_graph 去 50 goal 边 →
#   kg_loader convert --graph .work_hdcv/tg_holdout --out .work_hdcv/csr_holdout
# GT 闭包：
./build/bin/gar_reason --mode full --engine cpu --csr .work_hdcv/csr_holdout \
  --rules /ssd_data/zhuxk/graphs/KG/fb15k-237/rules --out .work_hdcv/fw_gt_closure_ho
# greedy 矩阵（9 配置 × 22 goal）：
bash .work_hdcv/fw_greedy_arm.sh        # budget=10 cap=8（地板）
bash .work_hdcv/fw_greedy_capsweep.sh   # budget=10 cap=64（地板）
bash .work_hdcv/fw_greedy_b50.sh        # budget=50 cap=64（主矩阵）
# bnb 三臂：
bash .work_hdcv/fw_bnb_arm.sh
# g3 抽查：
bash .work_hdcv/fw_g3_spot.sh
# 聚合：
python3 .work_hdcv/fw_aggregate.py
```
