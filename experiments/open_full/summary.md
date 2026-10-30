# open_full：全外延对比实验（backward 全枚举 vs forward 全量 chase）

日期：2026-10-30。预注册：NOTES.md。最小扩展 commit a49df84
（--backward-order score|random|hybrid、--backward-full-enum；默认
逐字节不变，synth 回归通过）。

## Setting 与基线

- fb15k-237 全基图（289,650 边）× 规则 1,652 子集；12 个开放 goal
  （9 个 r(x,B) + 3 个 r(x,y)；3 个子集下空答案 goal 剔除并记录）。
  GT = 基图 ∪ capped chase（10M 派生截断/4 轮；trunc_risk=possible
  的 GT 为下界，agent 超出部分记 beyond_gt 单独核验）。
- backward 臂：--executor backward --backward-full-enum，总预算
  10000 应用，collect/match cap 拉满，30min 超时，5 个排序配置
  （score / random×3 seed / hybrid-top8 窗口）。
- **forward 基线：一次全量 chase = 89.3s 端到端**（计算 ~11s +
  写盘 ~75s），峰值 8.8GB，产出 10M 事实覆盖全部 goal 答案集。

## 全矩阵（score 臂；random/hybrid 只在标注处分化）

| 层 | goal（GT/base） | recall | wall | spent | vs chase 89.3s |
|---|---|---|---|---|---|
| 小 | award_nominations(5/4) | **100%** | 0.62s | **1** | 快 144×，1 次应用 |
| 小 | awards_presented_winner(2/2) | 100% | ~0s | 0 | 答案全在基图（退化） |
| 小 | award_winning_work(1/0) | **0%** | 1.0s | 334 | **backward 推不出**（round-4 深度） |
| 小 | awards_won(6/0) | 100% | 6.4s | 7612 | 快 14×，但 99.9% 应用是失败 |
| 中 | profession(51/8) | **100%** | 20.2s | 951 | 快 4.4×；另 +904 beyond-GT |
| 中 | places_lived(243/0) | **0% / 51%** | 46s / 6-8s | ~6.4k / ~3.4k | **排序彩票**（见下） |
| 中 | art_direction_by(105/105) | 100% | ~0s | 0 | 退化（全基图） |
| 中 | languages_spoken(213/213) | n/a | — | — | 无规则以其为头，agent 报错退出；recall 恒 1（全基图） |
| 大 | film/language(2052/128) | 24% | 7.5s | 363 | fixpoint 卡 24%，chase 完备 |
| 大 | position(9624/2273) | 56% | 66.8s | 3109 | fixpoint 56%，chase 完备且仅慢 1/3 |
| 大 | release_region(857k/14k) | 2.8% | ~215s | 10000 | budget 顶满，chase 完胜 |
| 大 | contains(1.49M/5.5k) | 1.0% | ~218s | 10000 | budget 顶满，chase 完胜 |

random(3 seed)/hybrid 与 score 在 11/12 goal 上 recall 完全一致
（除 places_lived 与 film/language ±1pt 抖动）；hybrid ≈ score。

## 三个问题的答案

**a. time-to-full-answer / 完备率（分层）**：
- **小层（<10）**：backward 0.0003–6.4s 内 3/4 goal 达 100%（比 89.3s
  chase 快 14–144×）；但 **1/4 goal（round-4 深度的单答案）全臂 0%**
  ——深度超出 max_subgoal_depth=4 时 backward 在 fixpoint 静默失败，
  chase 无此问题。
- **中层（10–1000）**：分裂。profession 100%/20s；places_lived 在
  0%–51% 间摇摆在 46s/6–8s；两个全基图 goal 退化 100%。
- **大层（>1000）**：**无一大 goal 能完备**。≤万级答案 fixpoint 于
  24–56%（7.5–67s），十万级以上 budget=10000 顶满仅 1–3% 且耗时
  ~218s ≈ 2.4× chase。chase 一次 89.3s 全部完备。

**b. policy（backward 排序）**：全局几乎无差（11/12 goal 各臂一致，
hybrid 与 score 完全同形）；**唯一分化点 places_lived：score/hybrid/
rand_s13 全 0%（fixpoint 时 journal=0，6.4k 次失败应用），rand_s42/
rand_s7 各 51.4%（6–8s）**——score 序把搜索锁进一个高失败率规则族
（深递归 open-edge 子目标），随机洗牌 2/3 概率逃出。即：backward 的
指导排序在大 fan-in 目标上可能主动卡死，随机是有效 fallback，但也
只是抛硬币（1/3 seed 同样 0%）。

**复用率**：
- 跨种子（goal 匹配）共享派生 ≈ **0**：除 places_lived（3 条）外所有
  run 的 journal 中间事实为 0——每个答案独立从基图 1-hop 推出，无
  中间事实摊销（chase 的批量 join 正好相反，这是大层差距的机理）。
- 跨随机 seed Jaccard：大层 0.75–1.00（搜索被结构强约束，殊途同归），
  小层 0/1 两极（journal 只有 0–6 条）。

**soundness**：所有派生经 EvalAntecedent 门控（合法 chase 步）；
profession 的 904 条 beyond-GT 为 GT 10M 截断所致（30M 高帽闭包核
验见下）；全程零真实越界。

## 最终结论：全种子 backward vs forward 的边界

1. **答案集 ≤~50 且浅（≤round-3）**：backward 全枚举完胜（秒级 vs
   89s，且 100% 完备）。
2. **深度 >4 的小答案集**：backward 可能整体失败（0%），forward
   兜底——深度是比答案集大小更硬的 backward 边界。
3. **答案集 ≥~200**：backward 进入不完备区（24–56%），且时间逼近
   或超过 chase；**≥万级 chase 是唯一全外延路径**（复用率 0 决定
   backward 每答案付一次全价）。
4. policy 对全外延几乎无影响，但 score 序存在"卡死"风险点，
   random 多 seed 是便宜的保险。

## 复现

```bash
# GT（10M）与计时基线；30M 高帽核验闭包
./build/bin/gar_reason --mode full --engine cpu --csr <immutable_csr> \
  --rules <rules_subset> --out .work_hdcv/of_chase_time --threads 8 --print 0
# backward 臂（12 goal × 5 排序配置）
bash .work_hdcv/of_arm.sh        # --backward-full-enum --budget 10000 \
                                 # --open-collect-cap 10000000 --max-candidates-per-rule 100000
# 聚合
python3 .work_hdcv/of_aggregate.py   # → .work_hdcv/of_detail.tsv
```

明细：`experiments/open_full/detail.tsv`；goal/GT 分层：
`.work_hdcv/open_full_goals.tsv`；运行产物 `.work_hdcv/of_runs/`。
