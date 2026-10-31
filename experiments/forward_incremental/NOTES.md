# forward_incremental：增量 forward greedy 替换 naive（预注册）

日期：2026-10-31。已批准计划：plans/phantom-stranger-rocket-phantom-
stranger.md。判据先写死，不许事后移动球门。

## 变更边界（用户拍板）

- `--direction forward --executor greedy` 直接运行新实现；**无
  --forward-engine 回退、不保留旧 naive greedy 代码路径**。
- chase / backward / backward-bnb / forward bnb（单链证书）语义不变。
- 历史 naive 数字（experiments/forward_variants、open_full/full_matrix）
  仅作历史基线，标注"已移除实现"。

## 正确性判据（写死）

1. **回归**：cmake --build build -j 通过；synth chase/bnb/backward 的
   chain/closure/summary 逐字节一致（除新增统计字段）。greedy 允许不
   同但合法的推导链（调度顺序变了），但每条派生必须是合法 chase 步。
2. **delta 等价（synth）**：对每条规则、每个 fact version，
   "cache 中 embedding 集" == "该 version 上全量 Enumerate 后经
   已知结论/退役过滤的 embedding 集"；覆盖同规则多次引用同 slot、
   inverse slot、goal pins、重复事实插入、动态失败重 admit。
   验证方式：合成图上构造覆盖用例 + 在执行器中开 debug 校验模式
   （GAR_VERIFY_INCREMENTAL=1 时每个 delta 步对受影响规则做全量
   重枚举比对，不一致即 abort）。
3. **端到端**：新 greedy 派生 ⊆ CPU chase 闭包（synth + fb15k 抽
   样）；synth 无 cap 时 full-enum 答案集 == chase 答案集；cap/预算
   /超时导致的截断在 summary 明示；零真实 DIVERGENT。
4. TripleGraph 修改后所有公开读取返回最新数据；chase 每轮至多一次
   邻接重建、输出集合不变（对 .baseline 逐字节）。

## 性能判据（写死，fb15k-237 × 1,652 规则，串行）

- 工作量：embedding 枚举总数、全量枚举规则数 应显著低于 naive 的
  "每步 × 全 DG 可达规则"。
- 浅小 goal（goal 谓词闭包 ≤10 新事实）：端到端 wall 应优于历史
  naive（同 workload 参考值：budget=10 cap=8 时 naive 全 budget 耗
  尽 ~0.22s/goal 且 0 命中——新引擎以**命中+wall** 双重对比）。
- 记录 wall / 派生数 / recall / 终止原因 / 峰值 RSS / 各 counter。
- 若 wall 未改善：按分项耗时定位继续优化，不宣称完成。
- 不以"派生更少"为优化成功；不宣称新 forward 在所有场景优于 chase。

## 实验设计（性能部分，串行）

goal 选层（沿用 tasks50/open_full 口径，held-out 不必要——测的是
引擎行为非留出泛化；goal 在基图时 greedy 语义照常）：
- 浅小 ×2：award_nominations(_,/m/02flpq)（GT 5）、
  awards_won(_,/m/02r3zy)（GT 6）。
- 中等 ×2：profession(_,/m/03lgtv)（GT 51）、places_lived(_,/m/0nvt9)
  （GT 243）。
- 视耗时加 film/language(_,/m/04306rv)（GT 2,052）。
- 配置：--executor greedy --policy deterministic --scorer exact，
  budget=10000，cap=64 与 cap=100000 两档（对照 naive 两种口径），
  wall-limit 1800；chase 单独计时（已有 89.3s 基线，复核一次）。
- 历史 naive 基线数字：experiments/forward_variants/greedy_*_detail.tsv
  （budget=10 cap=8/64）与 open_full full_matrix（budget=1e9 cap=100k
  greedy exact 四 goal timeout 0.4%–33%）。

## 已知风险（写死）

- delta pins 枚举与全量枚举的集合差（PatternMatcher pins 语义的边界：
  多 edge 同 slot、inverse、自环）——用判据 2 的校验模式兜住。
- 惰性邻接可能破坏假设"邻接即最新"的调用方——构建后全仓 grep
  OutAdjacency 调用点逐一确认。
- HDC witness 门控不得改变 hdc scorer 数值（synth hdc 链回归）。

## 复核修正记录（2026-10-31，主代理复核后）

四点修复，判据不变：

1. **active 判定覆盖缺口**：旧 `!initialized || !cands.empty()` 使
   cands 空 + hold 全是 dynamic failure 的规则永久失活（除非 delta 恰
   好产生新 embedding）。修复：hold 中存在当前 version 可重准入的
   motif 时规则重新 active；仍全 retired 的 hold-only 规则不 active
   （防 deterministic 无计费空转）。端到端合成验证：`.work_hdcv/
   dyn_rules`（B: q(x0,x1) pattern + r(x0,x1) 动态前提 → s(x0,x1)）——
   修复前 no_candidates 且不命中（dyn_before），修复后 4 步推出
   s(a,b)（dyn_after）。
2. **GAR_VERIFY_INCREMENTAL 覆盖**：delta_update 记录全部触及规则，
   逐规则 verify（此前只验被应用的那条）。
3. **no-pick key 生命周期**：`!pick` 时不再清 cands（旧实现清 cands
   不清 keys，后续 delta 同 embedding 会被 dedup 误挡）；改为记录
   declined_version，图增长后恢复 eligible——与 failure registry 的
   重准入纪律一致，且无无计费空转。
4. **cap 语义精确化**（不重述判据，只精确化表述）：cap 是**每次枚举
   调用**的上限（规则初始化一次 + 每次 delta 枚举一次），cache 跨
   delta 累计可超过 cap。与旧 naive 的"每步全量枚举取前 cap 个"既不
   是子集也不是超集；与 NOTES 中"同 cap 语义"的表述差异在此明示。
   合理性：delta 枚举被 pin 到单条新事实，窗口极窄、几乎不触顶；
   只有用 per-call cap 才能保持每步成本有界。任何一次枚举触顶即计
   fwd_capped_enums，**capped>0 时不声称任何完备性**（fixpoint 仅
   对该可枚举空间而言）。
