# hitfix 实验进度 NOTES

## 2026-10-26 根因分析（已完成，证据在 logs/）

方法：/tmp/root_cause.cpp（不进仓库）——对每个失败任务，找出 head 谓词 ==
goal 谓词的规则，用 PatternMatcher 全量枚举（cap 500 万），定位"能一步推出
goal 的 embedding"在枚举序中的位置。

**根因（一句话）**：goal motif 从未进入候选列表——生成阶段的
--max-candidates-per-rule 8 把它截掉了，打分器根本没机会看到它。

证据（logs/rootcause_govt.txt, rootcause_profession.txt）：
- 任务 form_of_government(/m/05c74,/m/06cx9)：73 条规则的 head == goal 谓词，
  其中 34 条在基图上有一步派生（X 全过），但 goal embedding 的枚举位置全部在
  288~4306 之间，cap 8 全部截断（34/34 TRUNCATED，0 in-cap）。
- 任务 profession(/m/0161c2,/m/0dz3r)：1529 条 head 规则，61 条可一步派生，
  goal embedding 位置 414~16086，61/61 TRUNCATED。
- 即：cap 8 下候选 = 每规则枚举序前 8 个 embedding（按 slot pair 排序），与
  goal 无关；打分/decoy 分析是次要的——正确 motif 根本不在候选集里。
- 附带确认：这些 goal 边本身在基图中（hold-out 语义下被删），premise 均为
  基图边 → 全是 1 步可派生。

## 修法（进行中）

反向目标制导（用户建议方向，与证据吻合）：
1. graph::PatternMatcher 加 pins 重载（结论槽位常量绑定，默认行为不变）。
2. 新 BackwardExecutor：从 goal 反向——只对 head==goal 谓词的规则做 pinned
   枚举，候选空间塌缩到"能推出 goal 的 motif"；前提缺边且可派生时递归子目标。

## 2026-10-26 第二轮：BackwardExecutor 落地 + 20 任务复测

修复实现（commit ecaf034 + 后续）：
- `graph::PatternMatcher::Enumerate` 加 pins 重载（绑定结论槽位常量；默认路径
  回归逐字节一致）。
- 新 `BackwardExecutor`（--executor backward）：head 谓词索引 → pinned 枚举
  （候选空间塌缩到"能推出目标的 motif"）→ 打分排序 → EvalAntecedent 应用；
  缺边前提变子目标递归（单边/双边开放 + 自由变量协调），深度上限
  max_subgoal_depth=4。

结果（verify --tasks，shared-holdout，budget=10，cap=8）：
**18/20 CONSISTENT，0 DIVERGENT**（上一轮全部配置 0/20）。每任务 ~72-185ms
（对照：同 workload 的共享 chase 一次 227s）。

未命中的 2 个 = 仅有的两个 bstar_round=2 任务：
- film/actor(/m/015rkw,/m/0fg04)：chase 第 2 轮经 gar_009436 派生；其 pattern 缺
  nominated_for(/m/015rkw,/m/0fg04)——它自己是 round-2 事实（gar_004814 派生），
  而 gar_004814 的 pattern 需要 award_nominee(015rkw,x2) ∧ nominated_for(x2,0fg04)
  两条 round-1 派生边共享自由变量 x2 —— 需要跨派生协调 x2（join 两个各自由
  规则派生的中间事实），当前递归协调只覆盖"一条缺边"的主干情形。
- award_nomination/award(/m/016pns,/m/01by1l)：同类形态（award 递归族）。

结论：round-1 任务全部命中（18/18），round-2 任务差在"多条派生中间边的
自由变量协调"（记录为未来工作：join-coordinated subgoal derivation）。

## 2026-10-26 第三轮：攻坚 2 个 round-2 任务（timebox 内的迭代记录）

精确诊断（held-out 闭包 + GAR_DEBUG_BACKWARD 跟踪，logs/ 里的工具是 /tmp 临时程序）：
- film 任务（/m/015rkw→/m/0fg04）：chase 实际用 gar_005450
  （film(x0,x1) ∧ person(x1,x2) ∧ person^-1(x2,x3) → film(x0,x3)），**不是** 之前
  以为的 gar_009436。前提链：person(0fg04,028d4v) 由 gar_012469 第 1 轮派生
  （person 边在基图完全不存在），film(015rkw,x1) 第 1 轮派生，x1/x2 由基图
  person(x1,x2) 边连接。
- award 任务：gar_062307（award_winner^-1 ∧ nominated_for ∧ award_winner^-1 →
  award），同类形态。

本轮修复迭代（backward_executor.cpp）：
1. 开放边集合从"共享变量"放宽到任意边对（chase 链实际需要开两条不相邻边）。
2. pass-2 尝试按 (开放边到目标的 DG 距离 open_d, rel_fam, conf, index) 排序
   ——修复"索引序导致 prequel 族盲目试开"（带日志证据）。
3. 单边开放目标的 derive_collect（kMaxCollected=8）。
4. 子目标局部预算（每层减半）：递归族漫游（sibling 链）不再烧穿全局预算
   （spent 从超 budget 的 20 回到精确 ≤ budget）。
5. derive-first 2-open 快速路径：两条开放边各有一端被 goal pin 住时，先各自
   开放侧收集派生（不交基图枚举），再全模式重枚举校验。
6. 单边规则（r(x,y)→q(x,y)）的缺边直接变子目标（此前子模式为空被跳过）。

结果：这两个任务在 budget 10/30 下仍未命中。卡点（诚实记录）：
gar_005450 的 {film, person^-1} 双开需要在 budget 10 内完成
derive person(0fg04,·) + derive film(015rkw,·) + 应用，而同 open_d=1 层级还有
上百个 sibling/prequel 等递归族的失败尝试各烧 1-4 次计费。静态排序无法把
正确尝试排进前几位；下一步方向（写明）：给子目标派生加"派生可行性预检"
（pinned 枚举 0 匹配且无开放余地 → 免费跳过），或在 derive-collect 里
按 witness/DG 证据排序候选值。

## 第三轮收尾（timebox 到点）

v2 机制全部落地后复测 20 任务：**18/20 保持**（无回归，reports_backward_v2.tsv），
两个 round-2 任务 budget 30 仍不中。最终卡点分析（带跟踪证据）：
- film 任务：gar_005450 的 {film, person^-1} 双开已能被 derive-first 覆盖，
  但 derive-collect film(015rkw,·) 需要 nominated_for(015rkw,·)（本身 round-1
  派生、非基图）→ 子目标再递归，同层还有上百个 sibling/prequel 递归族的失败
  尝试分摊预算；budget 30 仍不够。
- award 任务：gar_000155 能反复派生 award(016pns, ·)，但目标 dst=01by1l 需要
  共享变量跨派生协调（两条中间边各派生各的、取值要对上），现有
  shared-free 协调只在"剩余子模式不覆盖共享变量"时触发，而这里剩余边
  nominated_for(x1,x2) 覆盖了 x2——走的是"双独立派生"路径，两侧取值碰不上。
  下一步（写入 summary.md 的未来工作）：双开放边在剩余模式覆盖共享变量时
  也要做"候选取值求交"（derive-collect 两侧再按剩余边过滤）。

## 2026-10-26 第四轮：双侧派生协调落地 → 20/20

机制实现（backward_executor.cpp，commit d380d16）：
1. **候选集口径修正（关键 bug）**：df 预检的 collect_candidate_values 此前把
   pattern 侧的 pinned 标志直接当 base 侧用——逆边（person^-1 等）pin 错侧；
   且预检不兼容时把自由端 pin 到未初始化的 v_hit=0（实体 #0 /m/027rn），
   白烧 6 单位/尝试。现统一 base 取向换算（pin_free/base_pin_src）。
2. **现成值并入候选**：候选 = Γ 中已存在（OutNeighbors 直读，免费）∪ 一级
   可派生（pinned 枚举预检，免费），现成值在前。chase 的 award 链 x1 就是
   基图边——只在"可派生"口径下永远看不到它。
3. **半连接求交（pair-join）**：候选对 (v1,v2) 经剩余子模式边过滤；中间边
   缺失但一级可派生（one_level_derives 免费预检）时才计费派生它——错误对
   零计费。先跑现成值 pass A，不成交才付费 derive-collect（pass B）。
4. **尝试排序加证据层级**：df 尝试按 tier 排（两侧候选均非空=1；求交可得
   组合对=0 提升），gar_005450 从第 18 位升到第 0 位，gar_062307 升 tier 0。
   拒绝条件收窄为"两侧非空且不组合且中间边永不可派生"（否则误杀
   gar_005450：其中间 person 边本身是 chase 派生事实）。
5. **子模式回退路径的计费门**：grounded 开放边先过免费一级预检
   （present || one_level_derives）才允许计费递归——film 任务的
   film(015rkw,0g56t9t) 型假子目标（基图 person 边枚举出的 grounding）从
   3 单位/个降为 0。
6. kMaxPairs=64（原 16 漏掉 vals1 第 14 位的正解 /m/04pk1f）。

结果（verify --tasks --shared-holdout，budget=10，cap=8）：
**20/20 CONSISTENT，0 DIVERGENT**（reports_backward_v3.tsv）。
- film 任务：spent=6/10（collect person 3 + 中间边派生 1 + film 定点派生 1 +
  verify 1）。
- award 任务：spent=2/10（x1 基图现成 → pass A 直接组合）。
- budget 敏感性：两任务在 budget=3 与 5 下均命中（film spent=4@b3、5@b5，
  award spent=2 不变）。注：成功路径 spent 可略超 budget（≤1，一次尝试内
  多次计费无中间检查点）——沿用既有计费口径，终止语义不变。
- 回归全绿：synth full 对 .baseline（summary 逐字节 + matches diff -r +
  closure 排序 md5 d2b83661…）；greedy 5a / bnb 5b chain.tsv 逐字节；
  verify synth：bnb 12/12、backward 12/12。
