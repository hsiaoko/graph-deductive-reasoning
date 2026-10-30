# forward_variants：forward agent 变体矩阵（预注册）

日期：2026-10-30。口径：全量 chase 无 policy/scorer（穷举），只做
ground truth 与计时基线；policy/scorer/HDC 属 forward agent（greedy/bnb）。

## 判据（写死）

1. scoring 影响：no-scorer（规则索引序）vs exact——命中/耗时差异即
   scoring 净效应；
2. policy 影响：deterministic vs random(3 seed) vs hybrid；
3. HDC 两处净效果：scorer=hdc(512) vs exact（主矩阵上轮已证无命中增益、
   更慢，本轮在子集 workload 复核）；bnb hdc-value vs utility。
4. soundness：派生 ⊆ 子集闭包（held-out GT 闭包，见下），
   零真实 DIVERGENT；默认行为逐字节回归。

## 表格式

每配置一行：命中/总数、time-to-goal mean/median/p90（主循环）、端到端
（含加载）mean、总派生数；bnb 加：展开数、best_cost/certified/chain 一致率。
逐 goal 明细 detail.tsv。

## 协议修正（2026-10-30 执行时发现并写死）

1. **Goal 是基图边**：Overall 50 goal 全部选自基图（50/50 均在
   triple_graph 中），gar_reason 无 holdout 会立即
   `goal_already_in_graph` 伪命中。对策：构造 shared-holdout CSR
   `.work_hdcv/csr_holdout`（G minus 全部 50 goal 边，289,600 边；
   中间产物 `.work_hdcv/tg_holdout`），全矩阵在该 CSR 上跑——与
   论文 §7 "held out X triples" 及 Overall --shared-holdout 口径一致。
2. **规则子集 = 1,652 条**（/ssd_data/.../fb15k-237/rules，目录
   gar_000000..gar_001651）；早前笔记中的"3,023"系误记。
3. **可行 goal = 22/50**：per-task holdout verify 判定 9（Overall bnb
   臂报告 goal_in_cpu_closure）+ 本轮 budget 探针补判 13；余 28 不可行
   （含 1 个谓词在子集无规则可推）。shared-holdout 复核 22/22 仍可行
   （feas_shared_holdout.tsv，INFEASIBLE=0）。
4. **GT 闭包**：`.work_hdcv/fw_gt_closure_ho`（held-out CSR × 子集，
   CPU chase，默认 cap 1M match/10M deriv，4 轮 10M 截断）。
   `.baseline/fb_deduce_cpu` 是**全图 2 轮**闭包，不能作本实验 GT。
   截断在 round-4 内部，截断子集随 merge 序（线程数）变化——单条
   成员判定以 verify --run 重算为准。
5. **greedy cap=8 地板效应在子集复现**：9 配置 × 22 goal 全 0 命中
   （20/22 budget 耗尽、2 no_candidates）——各配置无区分度。补
   cap=64 sweep（同 9 配置）作为区分度主表；cap=8 表保留为地板证据。
6. 可行性 sweep 出现 DIVERGENT=2：均为 agent `no_candidates` 提前终止
   （覆盖缺口），g23 唯一派生边已核对 ∈ GT 闭包（round 1）——非真实
   派生越界。

## 备注（已知边界）

- Overall 50 中子集可行 = 22（见上，非早先估计的 9）。
- greedy 在 rules_g3 全量下上轮 0/50 全灭（候选池被 cap 截断），主矩阵
  在 1,652 子集上；g3 抽查仅验证结论方向。
