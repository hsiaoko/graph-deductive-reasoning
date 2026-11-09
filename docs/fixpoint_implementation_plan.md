# FixpointExecutor 实现计划：Policy-Guided Deduction with Revival

对应论文原稿：`paper/policy_guided_deduction.tex`。~~本文是实现计划~~
**实现状态（2026-11-04）：步骤 1-4 已实现并验收**（见文末"实现状
态"）；步骤 5（action 级 learned policy / rollout 训练）**未实现**——
现有 BNB ValueModel 是分支级剩余成本模型，不能冒充 action-conditioned
Q。现有 greedy/bnb/backward 与其历史实验保持不变，新算法以
`--executor fixpoint` 接入。

## 1. 对新版算法的理解

输入为图 G、规则集 Σ、初始事实 Γ₀、goal g 和 policy π；输出为完整执行
trace、最终事实集 Γ、实际成本，以及 goal 是否推出。

核心不是“每步重新生成全部候选”，而是两个工作集和两个循环：

1. **主循环**：沿 goal 的依赖锥 DΣ,g 逐个展开 star。完整模式匹配产生
   action μ=(φ,h) 并进入 ready set R；policy 只决定 R 中 action 的求值
   顺序。成功则精确派生结论；静态前提失败则永久丢弃；仅缺动态前提时，
   将 action、绑定、已验证前提和缺失前提一起挂起到 suspended set W。
2. **fixpoint 循环**：主循环结束后，不再做策略选择；按轮检查 W 的缺失
   事实，条件到齐就复活并派生，已无生产者的等待项则删除。直到 W 为空，
   或一轮没有新事实。goal 在两个循环中都允许提前命中返回。

精确 GAR 求值保证 soundness；完整候选发现、公平调度和动态复活共同保证
可达 goal 的 goal-level completeness；有限 ground facts、action 去重和
单调增长的 Γ 保证 termination。有限 budget、match cap、wall timeout 下
只能报告相应截断状态，不能声称完备。

论文伪代码与示例有一处冲突：伪代码和 working-set 段落说 W 不参与 policy，
示例末尾却说 learned policy 在 fixpoint loop 重排 W。实现默认以伪代码为准：
**policy 只调度 R，W 由 fixpoint 机制复活**。若要让 policy 调度 W，需要
同步修改伪代码、成本口径与证明中的调度假设。

## 2. 现有代码的复用与差距

| 能力 | 现状 | 新版处理 |
|---|---|---|
| DG goal cone、距离、SCC | `DependencyGraph` 已有 | 直接复用；补 producer 视图，不改其分层 |
| CSR 模式匹配 | `PatternMatcher` | 复用同态、label 和逆槽位语义 |
| 精确谓词求值 | `graph::EvalAntecedent` | 复用单谓词求值，新增完整前提状态收集 |
| 结论 grounding | `ResolveRuleHead` | 复用，保持与 chase 一致 |
| 事实合并 | `AgentState::ApplyDerivation` | 复用，保持 Γ 单调与 journal |
| det/random/hybrid/Jev | `Policy` 已有 | 接入 R 的 action 级选择，不使用 rule-first 选择 |
| ready/suspended | 现有 per-rule cache/hold 不等价 | 新建 action 级 R/W 与完整生命周期 |
| 前提缓存 H | 现有 FailureRegistry 只记录整个 motif 失败 | 新建 ground-precondition 级缓存与版本语义 |
| 动态 revival | 现有实现按任意 Γ 增长重准入整个 motif | 改为记录全部 missing，按相关事实/轮次复活 |
| action-conditioned Q | 尚未实现 | 新训练与在线 policy；不能直接冒充现有 BNB value model |
| fair selection | 尚未实现 | fixpoint 执行路径加入 longest-waiting 公平调度 |
| shared sub-pattern reuse | 只有 per-rule candidate cache | 正确性完成后增加跨 star 的匹配复用 |

现有 BNB `ValueModel` 是分支级剩余成本模型，训练还把 off-path/failure
标为有限值 3；新版论文要求 action-conditioned rollouts、未观测动作不造
伪标签、失败 rollout 单独记录。因此二者不能直接视为同一 policy。HDC 或
numeric ridge 只可作为后续 action-value model 的可替换表示/回归后端。

## 3. 第一道语义验收：动态边位于 CSR pattern 的问题

当前很多挖掘规则把边依赖放在 Q 的 CSR 拓扑中，YAML antecedent 为空。
新版伪代码却要求：在不变的 G 上取得完整 Q 匹配，随后才等待 Γ 中的动态
前提；fixpoint 阶段不再匹配。

两者直接组合会漏掉 action：若 Q 中某条边尚待派生，初始完整匹配就不存在，
更谈不上把该 action 挂入 W。因此“每个 star 只展开一次 + 只对 G 完整匹配”
不能直接在现有规则表示上证明 goal-level completeness。

实现前必须固定一种语义，不能静默混用：

- **论文原语方案**：只允许动态前提出现在已绑定变量的 X 中，Q 是静态
  子模式。可逐字实现伪代码，但现有大量 chain 规则需要表示转换；若动态边
  引入新变量，还需 partial-match completion，不能只用 ground-fact revival。
- **推荐的通用 GAR 方案（semi-naive 修正版）**：保留现有 Q 语义，在
  G∪Γ 上做匹配；派生新事实后只对引用该 predicate 的 star 做 delta
  generation，按 (rule,embedding) 全局去重。fixpoint 交替执行 revival 与
  必要的 delta generation，直到没有新事实。它能覆盖现有规则，但论文需把
  “each star exactly once / no matching in fixpoint”改成“每个 ground action
  至多生成一次 / 只进行受新事实触发的增量匹配”。

建议采用第二种作为仓库的实际执行语义，并同步修订正文；若必须逐字对应
当前伪代码，则先限制/转换输入规则并用语义测试证明转换等价。

## 4. 模块与接口设计

### 4.1 新执行器

在 `engine/agent/include/gar/engine/agent/executor.h` 声明
`FixpointExecutor final : public Executor`，实现放
`engine/agent/src/fixpoint_executor.cpp`。在 `apps/agent_mode.h` 增加
`--executor fixpoint` 映射，旧 executor 默认值和输出保持不变。

私有 working-set 类型放 src-local 头，避免污染公共 API：

- `ActionRecord`：稳定 action id、Motif、RuleHead、前提状态、入队序号。
- `ReadySet`：action id → record，支持稳定枚举、删除和等待时间查询。
- `SuspendedAction`：action、已验证前提、全部 missing ground keys、
  复活所需的部分求值结果。
- `PreconditionCache`：静态 true/false 可永久缓存；动态 positive 可复用，
  动态 negative 必须带 Γ 版本或只作为本轮信息，禁止永久负缓存。
- `ProducerTracker`：至少按 predicate 跟踪未展开 producer、R/W 中 producer；
  只有确定没有剩余生产者时才删除等待项，避免破坏完备性。

### 4.2 主循环

1. 由现有 DG 距离筛出 goal-relevant rules，建立 leaf/derivable 标记和
   producer 索引；递归 SCC 不强行拓扑排序。
2. 每个 star 的 generation 只做 label/edge matching，产生去重 action。
3. 对当前 R 计算既有 `ScoredMotif`，调用 `Policy::Select`；不调用
   `SelectRule`，因为新版 policy 的对象是 grounded action。
4. 求值时收集全部前提状态，而非遇到第一个 false 就短路：
   - 全部成立：精确派生、记 trace、更新 producer/工作集；
   - 任一 leaf false：记录静态负结果，删除该 action，并按相同 ground
     precondition 安全剪枝；
   - 否则：保存 validated/missing，移入 W。
5. goal 命中即停；否则按固定 star 顺序继续。

### 4.3 Fixpoint revival

每轮扫描 W，只复查 missing；已验证静态/动态 positive 不重算。全部到齐则
复活并精确派生，随后更新相关等待项。没有新事实且没有待生成的 delta 时
达到 fixpoint。goal 命中同样提前返回。

若采用通用 GAR 方案，新事实只触发引用其 slot/mirror slot 的 star delta
匹配；复用现有增量匹配的 pin 思路，但 action 全局去重，且 revival 与
匹配终止条件合并计算。

### 4.4 公平调度与 learned policy

为 fixpoint 路径增加可配置 fair interval：每隔固定数量的策略选择，强制执行
等待最久的 ready action；其余时候调用 det/random/hybrid/learned/Jev。
公平选择也写 trace。只有 complete discovery + fair interval 开启 + 无有效
budget/cap/timeout 截断时，才输出 completeness 适用标记。

后续新增 action-level `LearnedPolicy`：

- 从已完成的执行保存 (S,g,μ) 和实际 remaining cost；
- 从记录状态对 alternative action 做 continuation-policy rollouts；
- 只给实际观测的 action 赋 cost target，失败 rollout 单独存储；
- 在线按预测 Q 排序，外层仍使用 DG hard filter、公平调度和精确求值；
- HDC-value 与 numeric-value 作为不同表示后端做公平 A/B，不预设 HDC 优胜。

### 4.5 成本与输出

吸取 backward 成本口径问题的教训，明确分离：

- `proof_cost`：最终成功证明链的应用成本；
- `selection_cost`：policy/scorer/model/Jev 控制成本；
- `evaluation_cost`：前提求值成本；
- `search_cost`：全部已计费动作，包括失败与挂起；
- wall time：单独统计，包含模型调用。

除现有 chain/closure/summary 外，fixpoint trace 需记录 select/evaluate/
suspend/revive/prune/derive 事件及 working-set 大小。termination 明确区分
`goal`、`fixpoint`、`budget`、`timeout`、`candidate_cap`。

## 5. 实施顺序

1. **语义门**：用现有动态链规则证明“只在 G 上完整匹配”会漏候选；冻结
   采用哪一种 Q/Γ 语义，并同步论文伪代码。
2. **最小正确版本**：实现 R/W、完整前提状态、H、producer 追踪和
   FixpointExecutor；先接 deterministic，旧 CLI 回归不变。
3. **调度与公平性**：接 random/hybrid/Jev，加入 longest-waiting 公平机制；
   保证 policy 只改顺序，不改逻辑结果。
4. **增量与复用**：若采用通用 GAR 方案，接入 delta generation；之后再加
   shared sub-pattern cache，并用全量重匹配逐集合对账。
5. **学习型 action policy**：实现 rollout 数据格式、训练隔离、模型加载和
   fair learned selection；与现有 BNB value experiment 分开报告。
6. **文档与实验**：更新 commands/agent_design/reasoning_pipeline；按新算法
   重跑对照，不沿用旧 greedy 数字冒充 fixpoint 结果。

## 6. 验证与验收

最小场景必须覆盖：

1. μ₂ 因 HighRisk(u) 缺失而挂起；μ₁ 派生该事实；fixpoint 复活 μ₂并推出 goal。
2. leaf 前提失败永久删除；共享该失败前提的 action 被安全剪枝。
3. 动态负结果不被永久缓存；相关事实到达后能够复活。
4. 递归 SCC 能终止；重复 ground action/事实不导致死循环。
5. 一个低分但必需的 action 在公平调度下最终执行；关闭公平性作为消融，
   不把 randomized policy 单独宣称为完备保证。
6. cap、budget、timeout 分别产生显式截断状态，不输出 completeness 标记。
7. 在无截断配置下，对采样 goal 与 chase 对照 goal reachability；所有派生
   均属于 chase 闭包，不同 policy 不改变可达结论。
8. 若采用 delta generation，对每个受影响 star 做全量重匹配集合对账；同时
   检查 trace 成本、proof cost 与实际派生链一致。

最终验收不仅看 goal-hit，还看派生 soundness、无截断时的 goal-level
completeness、termination、公平性和真实 wall/control cost。实现未通过
这些检查前，不把论文三个定理写成当前代码已经具备的性质。

## 实现状态（2026-11-04）

已实现并验收（语义 = §3 推荐通用 GAR 方案 + 用户确认扩展：CSR pattern
内缺失动态边也形成 partial action 挂起）。**2026-11-04 复核修正**：
① H 缓存键改为结构化 GroundPreconditionKey（完整逻辑 Predicate +
ground binding、full equality——旧 FactKey 键会合并 x.A>=18 与 x.A<18
等前提并错误共享剪枝；反例夹具 fixture_attr 验证修复）；② star 调度
改为逐 star（一次展开一个 star、policy 排空其 R、再下一个；delta 只投
给已展开 star，未展开 star 首轮全量枚举覆盖新事实；全部展开后才进
fixpoint 循环）；③ goal 命中立即返回（不再做无关 delta）；④ producer
存活性缓存按 Γ version + 记账代数双键失效（防 stale 错误剪枝）。

- `engine/agent/src/fixpoint_executor.cpp`（只构造并运行 session）+
  src-local `fixpoint_session.{h,cpp}`（一次 Run 的全部工作集状态，短成
  员函数：GenerateStar/AddComplete/AddPartials/OnDerivation/Evaluate/
  RevivalPass/SelectReady/Derive——行为保持重构，与 main_verify_revival/
  main_verify_star 参照 trace/closure/summary 逐字节一致）+
  `fixpoint_matcher.{h,cpp}`（可挂起动态边计算、子模式 remap、部分匹配
  扩展）、`fixpoint_working_set.{h,cpp}`（R/W、稳定 id、等待序）、
  `fixpoint_preconditions.{h,cpp}`（逐前提状态收集、H 缓存：静态永
  久/动态 positive 可复用/动态 negative 带 Γ 版本）、
  `fixpoint_producers.{h,cpp}`（producer 存活 least-fixpoint，保守剪
  枝）。公共头只增 `FixpointExecutor` 声明与 `AgentResult::fx_stats`
  （可选字段，旧执行器默认输出不变）。
- CLI：`--executor fixpoint`、`--fair-interval N`；旧 CLI 回归不变
  （athletes bnb utility 展开数 2084 逐数复现今日已验证值、synth
  bnb/backward 语义通过、dyn 增量 greedy 4 步命中保持、numeric value
  model 校验 1.8e-12）。
- 验收（fixture 与日志在 .work_hdcv/fixpoint_checks/）：
  1. 挂起-复活主路径：open-side pattern 边挂起（r2 缺 p(b,*)）经
     delta 补全；both-bound 挂起（r5 缺 u(a,d)）经 delta 完成推出
     goal g(a,d)（fx_run6）。
  2. 求值级动态挂起→fixpoint 复活：q+p(antecedent)→u 先于 p 派生被
     求值→挂起，p 到达后 fixpoint pass 复活派生（fx_runB：
     select/suspend/derive/revive/derive/goal 全序）。
  3. 静态假永久剪枝 + H 共享剪枝（r8 新鲜求值 prune；r9 的同一 ground
     前提 zzz(a,b) 经 H 命中标记 prune-cached，不重新求值）。
  4. 动态负缓存带版本：缺失前提以 Γ 版本记录，图增长后重查（复活即
     证据）；SCC（u↔m 循环）经事实/action 去重终止（fx_run5b
     fixpoint 终止，goal 未推出，与 chase 闭包一致：g(a,b) 不在闭包）。
  5. 公平调度：--fair-interval 1 产生 fair_select 事件且结果不变
     （fx_fair）；det/random(2 seeds)/hybrid 同逻辑结果（同派生集）。
  6. 截断状态：budget=2 → budget；cap=1 → candidate_cap（15 次触顶
     计数）；wall-limit → timeout。
  7. 对账：fixpoint 全部派生 ⊆ fixture chase 闭包（0 越界）；goal
     可达性与 chase 一致（g(a,d) 可达/g(a,b) 不可达）。
- 未实现（不声称）：action 级 learned policy（rollout 训练/模型加载）、
  policy 调度 W（与伪代码一致，W 仅 fixpoint 复活）、shared
  sub-pattern 跨规则复用（计划 §4 列为正确性后优化）。

## 性能复核与递归正确性（2026-11-05）

对照 `CpuChaseEngine` 后确认：二者复用同一个 `PatternMatcher`；Chase 的优势
来自逐规则、逐轮批量计算与 `MergeRoundFacts`，而非天然拥有更少的 ground
matches。旧 Fixpoint 还在每次选择时重算整个 R，并在每条派生边后扫描全部
plan/W，二者的复杂度不能按“一条规则”等同。

- 新增 fx 分项计时。p00 的 60 秒旧实现 profile：selection=58.7569s，
  其中 score=52.6081s；21,165 次选择累计评分 88,453,319 个候选，
  `revival_passes=0`，瓶颈明确在 main loop，而非最后的 fixpoint loop。
- exact/null 改为候选分数缓存 + 新 witness fact 订阅更新；R 使用 id→位置
  索引，deterministic/hybrid 使用保持当前位置同分顺序的惰性堆；random 保持
  softmax 分布。HDC/Jev 不使用不满足其更新语义的缓存。
- delta 使用 slot→plan 索引；partial W 使用 ground/open-side WaitKey 索引。
  在真实图 p00 上，缓存和索引阶段的执行链保持前缀一致，堆阶段与索引阶段
  chain/closure/trace 逐字节一致。优化后的 input-order p00 在 5.395s 推出
  goal；旧 900s 运行为 timeout。
- 发现真实正确性反例 p04：旧 goal pinning 在递归 goal 上排除了同谓词的
  中间派生，导致 cap=0、正常 fixpoint 却漏掉 Chase round-2 goal。单规则
  `gar_000798` 可独立复现。修复为：goal 谓词属于递归 SCC（含自环）时，
  不对 goal-headed 规则施加最终常量 pin；非递归 goal 保持 pin。修复后单规则
  及完整 512 规则的 p04 均推出 goal；后者约 0.530s、3773 派生、cap=0。
- `--fixpoint-star-order input|goal` 将规则顺序与 goal-DG 距离顺序分开；
  默认 input 保留原调度。`goal` 稳定排序，非截断时仍展开所有 cone star。
- 日志分为 `.work_hdcv/fixpoint_opt/profile_baseline`、`ready_cache`、`indexed`、
  `heap`、`recursive_safe`；完整 goal/negative 矩阵使用独立 `runs_goal`。
  早期 fx_run* trace 可能属于一次性全 star 展开的旧语义，不作当前顺序的
  字节基准；最终验收使用 Chase goal reachability、派生集合 soundness、cap 和
  termination。

完整复验报告：`.work_hdcv/fixpoint_opt/report_goal/summary.md`（85/85 计划
run，状态 `complete_with_truncation`）。Fixpoint-D 的 12 个正例全部命中；
宏平均 3.902s，低于 Chase cold 14.248s，但高于批量摊销 Chase T/12=1.187s，
两种口径不得混用。40 个 det run 中两个负例因 cap 未解决（n01=47 次、
n02=1 次），不是完整否定证明。Fixpoint-R 的 p10 三个 seed 均在 900s 内部
限时终止，故正例命中率为 11/12；其宏平均 89.812s 包含这些 timeout。
全部 76 个 Fixpoint run 的派生均属于 rules512 Chase GT，三次 Chase closure
逐字节一致。Fixpoint 最大进程树 RSS 1.905GiB；包含 BNB 基线的总峰值为
6.388GiB，最长 run 901.991s，未超资源上限。最终构建、真实图回归、
hybrid/null/HDC-fallback 小图和 value-model 校验通过；项目没有 CTest 用例。

## Random 的 Gumbel-max 增量调度（2026-11-06）

上述 p10 超时的直接原因不是派生或模式匹配，而是旧 Random 每次选择都扫描完整
R、重建权重并执行 softmax。三个旧 run 的 selection 分别为 896.392s、896.043s、
895.553s，占约 900s 总时限的 99% 以上；评分本身均不足 0.73s。

当前 exact/null 增量路径改为 Gumbel-max：action 首次评分时生成并保存独立噪声
`g_i ~ Gumbel(0)`，重评分时复用该噪声，以 `total_i/tau + g_i` 作为堆优先级。
独立 Gumbel 噪声满足

\[
\Pr\left(i=\arg\max_j(total_j/\tau+g_j)\right)
=\frac{\exp(total_i/\tau)}{\sum_j\exp(total_j/\tau)},
\]

因此选择分布与原 softmax 相同，但可复用 deterministic/hybrid 的惰性堆，不再逐次
构造全 R 的分数/权重数组。action 删除时释放噪声；HDC/Jev 的完整评分路径不变。
固定 seed 对当前实现可复现，但不要求与旧实现有相同的随机轨迹。

p10 单目标、三个冻结 seed 的事后复验（不覆盖完整矩阵报告）：

| seed | 旧 wall / selection | 新 wall / selection | 新派生 | 结果 |
|---|---:|---:|---:|---|
| 7 | 901.724s / 896.392s | 13.494s / 0.812s | 28,355 | goal，cap=0 |
| 13 | 901.724s / 896.043s | 11.260s / 0.735s | 27,059 | goal，cap=0 |
| 42 | 901.724s / 895.553s | 11.007s / 0.793s | 26,603 | goal，cap=0 |

三个新 run 的全部派生均属于 rules512 Chase GT（outside_chase=0）；最大进程树
RSS 2.580GiB。最终二进制的 p04 deterministic chain/closure 与此前正确结果逐字节
一致。日志位于 `.work_hdcv/fixpoint_random_opt/runs`。这里只重跑 p10 和 p04，
不能把其他 goal 的旧 Random 计时拼成“新实现完整矩阵”；完整矩阵的 0.917/89.812s
仍是上述历史配置的结果。

## 24-goal 时间/候选工作量扩展实验（2026-11-06）

按 `experiments/fixpoint_policy/GOAL24_PREREGISTRATION.md` 冻结输入：rules512 不变，
保留 12 个锚点并新增 12 个 goal，round 1/2/>=3 各 8 个；只含可派生 goal，不混入
负例拒绝测试。D/R 各三重复，加三次 Chase 共 147 run。两个运行阶段合计约 606s，
单 run 最长 20.865s，最大进程树 RSS 2.580GiB，均低于预设上限。

为统一“分支”口径，Chase 新增全轮 `embeddings_total`（末轮 per-rule 计数不变），
Fixpoint 新增 `fx_match_embeddings` 和 `fx_ready_insertions`。这里比较的是物化的
label-valid 完整匹配，不是搜索树节点，也不是最终派生数。

| 方法 | goal hit | macro wall | macro 完整匹配 | macro 求值 | macro 派生 |
|---|---:|---:|---:|---:|---:|
| Fixpoint-D | 24/24 | 3.630s | 321,659 | 18,998 | 18,998 |
| Fixpoint-R | 24/24 | 3.916s | 334,442 | 17,973 | 17,973 |

三次 Chase 中位数 15.408s，完整匹配 306,101,189，去重前派生候选 3,253,480，
新事实 349,552。D/R 的单目标 cold 加速分别为 4.244x/3.935x，完整匹配减少
99.895%/99.891%，派生减少 94.565%/94.858%。所有 144 个 Fixpoint run 均命中、
cap=0、timeout=0，派生全部属于冻结 Chase GT；三次当前 Chase closure 与 GT
逐字节一致。

批量口径不能省略：24 个 goal 的 D/R 中位数时间之和分别为 87.128s/93.983s，
是一次完整 Chase 的 5.655x/6.100x；一次 Chase 可直接回答全部 24 个 goal。
因此本实验支持“单目标显著减少候选和冷启动时间”，不支持“批量目标总时间优于
一次全量闭包”。完整报告与逐 run 数据位于 `.work_hdcv/fixpoint_goal24/`。

### 扩到 36 goal（同一 rules512）

在 24 个冻结 goal 上新增 p24--p35，三层各增加四个；复用原 147 run，仅执行 72 个
新 run。新增阶段约 333.5s，两轮运行累计约 939.8s，仍低于 1000s。完整矩阵 219/219
已观测，D/R 均为 36/36 命中、soundness=1、timeout=0：

| 方法 | macro wall | macro 完整匹配 | macro 求值 | cold 加速 | 36-goal 批量/Chase |
|---|---:|---:|---:|---:|---:|
| Fixpoint-D | 3.778s | 251,373 | 17,914 | 4.079x | 8.827x |
| Fixpoint-R | 4.337s | 265,231 | 18,942 | 3.553x | 10.133x |

最长单 run 30.557s，最大进程树 RSS 2.580GiB。新 goal p32 的六个 run 各触发一次
candidate cap（D/R 各三次），虽然命中且派生正确，仍是截断结果，不能用于完备性
声明；其余 210 个 Fixpoint run 无 cap。报告状态为 `complete_with_truncation`。

新增 goal 改变了宏平均的样本混合，不能把 24/36 两组宏平均当作严格的规模单调曲线。
单目标结论仍成立，批量总时间仍明显慢于一次完整 Chase。完整数据位于
`.work_hdcv/fixpoint_goal36/`，配置为 `experiments/fixpoint_policy/GOAL36_PREREGISTRATION.md`。
在当前“与历史运行合计不超过 1000s”的预算下，goal 扩展宜停在 36；增加规则会改变
GT，应另起独立实验，不能复用本轮闭包或把不同规则集的加速比直接拼接。

### 独立 rules640 实验：规则增加，Chase 与目标制导的成本分化

按 `experiments/fixpoint_policy/RULES640_PREREGISTRATION.md` 冻结新增 128 条规则，
图和 36 个 goal 不变。三次 Chase 均达不动点、闭包逐字节一致且覆盖全部旧 goal；
旧闭包也是新闭包子集。规则仅增加 25%，但全轮匹配增长 42.604x、派生增长 14.359x、
Chase 中位 wall 增长 48.187x，说明新增规则触发了大量递归/中间事实计算。

| 方法 | rules512 宏平均 wall | rules640 宏平均 wall | 倍数 | rules640 cold 加速 |
|---|---:|---:|---:|---:|
| Fixpoint-D | 3.778s | 11.104s | 2.939x | 66.863x |
| Fixpoint-R | 4.337s | 11.698s | 2.697x | 63.467x |

rules640 的 D/R 宏平均完整匹配为 455,325/472,083，求值与派生为 45,616/46,028；
两者均 36/36 命中、timeout=0、soundness=1。更关键的是，36-goal 中位数时间之和为
399.750s/421.142s，低于一次 Chase 742.462s，批量加速 1.857x/1.763x。上一轮
rules512 的批量口径是慢 8.827x/10.133x，因此这个冻结 workload 出现了批量口径反转：
目标制导避免的全规则递归计算超过了逐 goal 的重复装载和控制成本。这不是普遍的
规则数量定理，也不保证其他 640 规则子集有相同拐点。

正确性限制：p11/p21/p32/p34 共 24 个 Fixpoint run 有 cap，总 cap 54（D/R 各 27），
截断命中不用于完备性声明。最长 Fixpoint run 200.459s、最长 Chase run 835.628s，
最大 Fixpoint RSS 5.412GiB，单个 run 均低于限制；全部 219 个进程 wall 总和为
4726.016s，整批并非 1000s 内。完整报告见 `.work_hdcv/fixpoint_rules640/summary.md`。
