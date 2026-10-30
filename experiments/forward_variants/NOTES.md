# forward_variants：forward agent 变体矩阵（预注册）

日期：2026-10-30。口径：全量 chase 无 policy/scorer（穷举），只做
ground truth 与计时基线；policy/scorer/HDC 属 forward agent（greedy/bnb）。

## 判据（写死）

1. scoring 影响：no-scorer（规则索引序）vs exact——命中/耗时差异即
   scoring 净效应；
2. policy 影响：deterministic vs random(3 seed) vs hybrid；
3. HDC 两处净效果：scorer=hdc(512) vs exact（主矩阵上轮已证无命中增益、
   更慢，本轮在子集 workload 复核）；bnb hdc-value vs utility。
4. soundness：派生 ⊆ 子集闭包（.baseline/fb_deduce_cpu，648,595 事实），
   零真实 DIVERGENT；默认行为逐字节回归。

## 表格式

每配置一行：命中/总数、time-to-goal mean/median/p90（主循环）、端到端
（含加载）mean、总派生数；bnb 加：展开数、best_cost/certified/chain 一致率。
逐 goal 明细 detail.tsv。

## 备注（已知边界）

- Overall 50 中子集可行 = 9（fb15k_overall 判定）；同口径可补到 ~15。
- greedy 在 rules_g3 全量下上轮 0/50 全灭（候选池被 cap 截断），主矩阵
  在 3,023 子集上；g3 抽查仅验证结论方向。
