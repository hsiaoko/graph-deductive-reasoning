# open_full：全外延对比实验（预注册）

日期：2026-10-30。用户拍板 setting 如下，判据先写死后执行。

## Setting

- 图：fb15k-237 全基图（289,650 边；open goal 求外延不删边，沿用
  open_goals G1/G2 口径）。规则：1,652 条子集
  （/ssd_data/zhuxk/graphs/KG/fb15k-237/rules）。
- Goal：open_goals 清单复用——G1 r(x,B) 10 个 + G2 r(x,y) 5 个；
  其中 3 个 G1 在子集闭包下答案集为空（countries_spoken_in/08809、
  participating_countries/0dlhg、honored_for/07nxvj），剔除并记录。
  最终 12 个 goal（明细 .work_hdcv/open_full_goals.tsv）。
- 分层（按子集 GT 答案集重算，非 g3 口径）：小<10 / 中10–1000 /
  大>1000。当前分布：小 4、中 4、大 4。
- Ground truth：基图 ∪ 子集 capped chase 闭包
  （.work_hdcv/fw_gt_closure，10M 派生截断、4 轮、~89s）。截断止于
  round-4 内部 → 谓词闭包 max_round≥3 的 goal 标 trunc_risk=possible
  （GT 为下界，召回可能高估；单独标注）。soundness 另由
  EvalAntecedent 门控（每步都是合法 chase 步）+ 派生 ⊆ GT 闭包抽检。

## 方法臂

1. **forward 全量 chase**：一次闭包（计时基线 + GT）。实测 ~89s
   （<200s ✓）。
2. **backward 全外延（deterministic=score 序）**：`--executor backward`，
   总预算 10000 应用（per-seed 小预算由递归 local_cap 折半内建承担），
   --open-collect-cap 拉满（全枚举），match cap 放大，单 goal 30min
   超时。
3. **backward 换序**：score（=臂2）/ random（3 seed：42/7/13）/
   hybrid（score 窗口内随机，top-8）。backward 执行器原生不查 policy，
   用最小扩展 `--backward-order score|random|hybrid`（默认 score 逐字
   节不变，单独 commit）。

## 指标与判据（写死）

- 每 goal × 臂：wall、完备率（recall = |答案 ∩ GT|/|GT|，另报
  derived-only recall = 对"非基图答案"的召回）、总派生、spent。
- 分层下结论：backward 全外延 time-to-full-answer（或超时完备率）vs
  chase 一次 ~89s——小/中/大三层各自谁优，**超时/不完备即数据**。
- 复用率：a) 中间事实占比（派生中谓词≠goal 谓词的部分——为多个答案
  共享的前提）；b) 3 个 random seed 间派生集 Jaccard。
- soundness：派生 ⊆ GT 闭包（全谓词），零真实越界；2 个 goal 抽
  verify --run。
- 峰值内存：/usr/bin/time -v 记录 max RSS。

## 已知边界

- 大答案集 G2（86 万、149 万）budget=10000 下完备率上限 ~1%——
  预期 forward 完胜，属 case 1 验证而非新发现。
- languages_spoken/art_direction_by 答案全在基图（round 0）——
  退化 goal（recall 恒 1），保留作对照并标注。
