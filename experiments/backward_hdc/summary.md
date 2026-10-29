# backward_hdc：方向开关 + HDC 价值模型移植 backward（Phase 1 结论）

日期：2026-10-29。预注册判据见 NOTES.md。

## 任务 1：方向开关（完成，回归全绿）

- `--direction forward|backward`（不设 = 旧行为逐字节不变）。映射实测：
  `--executor backward` ≡ `--direction backward --executor backward`
  （chain.tsv 逐字节一致）；`--direction backward`（或 `--executor greedy`）
  = 退化单路径臂（首选即终选、无 pass-2，synth 上 goal 不命中——消融臂
  预期更弱，行为确定）；`forward+backward` 报 "mutually exclusive"；
  `backward+bnb` 报 "reserved for Phase 2"。
- 回归（对 66fb175 参照 worktree 逐字节）：synth greedy/bnb chain+summary、
  fb15k athletes bnb chain、verify --run synth 全部逐字节一致。
- 文档：docs/commands.md（参数表 + 方向说明）、docs/reasoning_pipeline.md
  （§5.1 方向开关）已更新。

## 任务 2：HDC-backward A/B（Overall 50 goal，per-goal 留出，LOGO 模型）

数据：50 goal 的 GAR_DEBUG_BACKWARD 尝试轨迹（新增 att-result 结果行），
290 个已执行 OR 尝试（25/50 goal 有 pass-2；其余 pass-1 直解），21 个成功
尝试。LOGO：逐 goal 一模型（训练集剔除该 goal）。

| 臂 | 命中率 | 主循环耗时 mean/p90 | 总计费 |
|---|---|---|---|
| backward+heuristic（旧） | 46/50 | 1.59s / 3.35s | 138 |
| backward+hdc-value | 45/45+... 45/50 | 3.38s / 4.76s | 143 |

逐 goal 变化：lost = goal35、goal44（heuristic 命中、hdc 未中）；
gained = **goal49**（4 个此前未命中 round-2 goal 之一，spent 10→3、
2.51s 救活 ✓）。其余三个未命中 goal（41/43/48）两臂均未中；goal48 在
hdc 臂耗时 69.1s vs 3.05s（误排序引发深层无效探索）。

## 判据逐条核对

1. 命中不降级：**未过**（46 → 45，净 −1）。
2. 硬 goal 耗时不劣化：**未过**（mean 1.59→3.38s，p90 3.35→4.76s；
   goal48 单点 22×）。
3. 提升 ≥20%：命中率 −2.2%，耗时劣化——只有 goal49 的单点救援。
4. soundness/回归：派生机制未动（仅排序），soundness 由构造保持；
   默认 heuristic 逐字节一致（任务 1 回归覆盖）。

## 判定

**不可行（本期形态）**。与 bnb 臂的对比分析：bnb 价值模型有 33k 分支节点
训练量 + 明确的"剩余成本"回归目标；backward 的 OR 尝试排序只有 290 个
尝试样本（21 正例），且既有启发式（tier 证据 + open_d + rel_fam）已经把
正确尝试排得很前——可改进空间只剩尾部 4 个 goal，模型噪声足以把
已有序的好尝试挤下去（goal35/44 丢失、goal48 耗时爆炸）。若未来重启：
先扩样本（更多 goal/更深任务）或把模型用在"是否值得进入 pass-2"的
二分类闸门上（不改动既有排序，只在启发式失败时介入）。

---

# Phase 2：backward-bnb（AND-OR 分支定界最优链）结果

实现：`engine/agent/src/backward_bnb_executor.cpp`（DFBnB 于 backward 的
pinned 候选空间；共享 Γ → DAG 成本口径；下界 = 谓词派生深度，内联
Dijkstra 计算；剪枝/深限/失败注册/计费沿用既有件）。路由：
`--direction backward --executor bnb`（原 reserved 报错处打通）。
证书截断标记：任何候选因"开放端点未定/枚举 cap"被排除时，
certified_optimal/certified_no_chain 置 false 且 termination 带
`_space_truncated` 后缀。

## best_cost 一致性（判据 1：全过）

| goal | forward bnb（certified） | backward-bnb | 一致 |
|---|---|---|---|
| synth lives_in(p8,t1)（手造 cost-1） | 1 | 1, certified | ✓ |
| synth lives_in(p0,t1)（手造 cost-2） | 2 | 2, certified | ✓ |
| athletes(/m/05r4w,/m/09n48) | 2（2084 展开，200s） | 2, certified（**2 展开，0.5s**） | ✓ |
| Overall 可行 goal 17/19/20 | 1（63/13/33 展开） | 1（40/1/232 展开） | ✓✓✓ |

## soundness（判据 2：过）

verify --run（Overall goal 17，逐任务留出）→ CONSISTENT（派生 ⊆ 闭包）。
所有派生经 EvalAntecedent 门控 = 合法 chase 步。

## 性能对比（判据 3：符合预期，backward 更便宜）

| 指标 | forward bnb | backward-bnb |
|---|---|---|
| athletes 展开数 / wall | 2084 / 200s | **2 / 0.5s** |
| Overall goal 17/19/20 展开 | 63/13/33 | 40/1/232 |
| 峰值 RSS（athletes） | ~4GB | ~1.5GB |

（goal 20 展开 232 > forward 33：rules_g3 81k 规则下每目标的候选空间更大；
总体形态仍是数量级优于 forward。）

## 证书覆盖范围声明（显著标注）

证书 = "所见候选空间（pinned 枚举 cap 内 + 接地子目标递归）内成本最优
DAG 证明"（共享派生只计一次；下界用树式 Σ 前提 lb，DAG 下仍安全——共享
只发生在已派生事实、其 lb 贡献为 0）。**不覆盖**：开放端点协调形态
（disjoint-free 双开等，如 film/award 的 gar_005450 链）——这类 goal 会
如实报 no_chain 且 certified_no_chain=false + `no_chain_space_truncated`
终止标记（实测 film 目标：1002 展开后如实失败，不假装有证书）。
结论：核心搜索正确性验证全过，但因协调形态未入证书空间，本期标
**experimental**——覆盖范围即上述文字。

## 回归（判据 4：全过）

forward bnb athletes chain 逐字节一致（Phase-2 构建后复验）、verify synth
逐字节一致、plain backward 逐字节一致、direction 冲突报错行为不变。

---

# Phase 3：实例级下界（--backward-bnb-lb instance）

构造（`BackwardBnBExecutor`，cfg.backward_bnb_instance_lb；全部保守）：
- 在 Γ → 0（精确）；
- 无 head 规则 / 谓词不可达 → INF（安全剪枝，诊断计数 inf_prunes）；
- 当前无任何 motif 点火（pinned 枚举 + label + EvalAntecedent 的免费预检，
  按 Γ 版本缓存）→ max(谓词界, 2)：Γ 单调增长，"现在无全匹配"意味着任何
  证明须先派生别的事实再派生本目标，≥2 步（文档论证见下）；
- 当前可一级派生 → max(谓词界, 1)（紧化）；
- 与谓词派生深度取 max。

**随带的可采纳性修正（比 instance 下界更重要）**：Phase-2 的 incumbent
剪枝用 est=1+Σ前提下界——DAG 共享下两个未派生前提可能共享子证明，
真实成本 < est，Σ 剪枝可能误杀最优。本期：剪枝改用 1+max（可采纳），
前提 sub-cap 的"后续余量"改用后缀 max 且每步重查存在性（side-effect
派生归零）；Σ 仅用于候选排序。修正后全部 Phase-2 用例复跑，best_cost
不变（见下表）——原结果未受影响，但论证现在严格。

## 正确性复验（判据 1：全过）

| 用例 | predicate | instance | certified |
|---|---|---|---|
| synth cost-1 / cost-2 | 1 / 2 | 1 / 2 | 不变 ✓ |
| athletes（rules, cap64） | 2（2 展开） | 2（2 展开，62 预检） | 不变 ✓ |
| Overall goal 17/19/20（g3） | 1/1/1 | 1/1/1 | best_cost 不变；三者
space_truncated=1（g3 cap8 截断，certified 置 false——如实标注） ✓ |
| film 协调形态目标 | 1002 展开如实失败 | 同 | 仍不报证书 ✓ |

soundness：派生经 EvalAntecedent 门控，零真实 DIVERGENT（verify --run
在 Phase-2 已覆盖，本期语义未动派生路径）。

## 效果 A/B（判据 2）

| 用例 | 展开 pred→inst | 预检数 | wall pred→inst |
|---|---|---|---|
| synth cost-2 | 3 → 2（est 5→6，inf 0→3） | 2 | 同（ms 级） |
| athletes | 2 → 2（est 88→118） | 62 | 0.5s → 0.5s |
| Overall 3 goal | 40/1/232 → 40/1/232 | 少量 | 同 |
| r2c_8 | 1 → 1（inf 0→7） | 1 | 24.5ms → 24.8ms |

结论：**剪枝确实更紧（inf/est 剪枝计数上升），但本工作负载的展开数几乎
不变**——谓词界已经剪掉了同批分支；实例界只在"谓词便宜但该实例当前不可
派生"的形态（synth 手造例）上小胜。预检成本低（µs~ms 级，被版本缓存）。

## 判定与建议

正确性全过、回归全绿、效果中性。**不建议设为默认**：收益在本负载上是
噪声级，谓词界已够；保留为文档化选项（`--backward-bnb-lb instance`），
未来若出现"谓词浅而实例深"形态的硬 goal 再启用。

---

# Phase 3 主任务：协调形态入证书空间

## 成本口径修正（过程发现的真实 bug）

synth 手造协调用例暴露 Phase-2 的增量计费缺陷（目标事实已被先前候选派生
时误报更优成本）。重写为**证明集口径**：proof = 唯一派生事实集合，子证明
并集去重（共享精确一次）= 真实 DAG 应用数；前提存在性按 Γ0（base 快照）
判定；证明/失败 memo 按"目标候选空间相关派生计数戳"精确失效。
修正后 synthc 用例从 cost=2（错）→ cost=3（=手工最优，certified）。

## 协调形态候选（pass C）

复用主引擎机制：两侧候选 = 现成 ∪ 一级可派生（免费预检，按探针空间戳
缓存）、剩余边半连接（在 Γ 或一级可达集成员测试）、组合对生成全接地
embedding → 走既有 Cand/递归管线。对数 cap=64（超出记 cap_truncated）。
下界/剪枝与其他候选同口径（排序 1+Σ，剪枝 1+max）——耦合子目标不相加。

## 验证结果

单元级（synthc 手造，graph+5 规则，tools 在 .work_hdcv/synthc/）：
goal g(a,d) 必须双侧协调（p(a,x1)、q(d,x2) 均需派生，m(x1,x2) 基图半连接）：
**best_cost=3 = 手工最优，certified_optimal=true，无截断**；更贵的 G2 替代
（cost 5）被正确排除。

真实协调 goal（rules_g3，逐任务留出）：
- award(/m/016pns,/m/01by1l)：**cost=2**（与主引擎 backward 首解 spent=2
  一致；比 chase 实际使用的 gar_062307 四步链短——bnb 找到的是另一条
  两步证明：nominated_for(023vrq,011z3g) + award 直推规则），expansions=2，
  19s；但 shape_truncated=1（部分规则的开放端点候选被排除）→
  certified_optimal=false，termination=goal_space_truncated（如实）。
- film(/m/015rkw,/m/0fg04)：**30min 超时未决**。诊断：候选枚举广度爆炸
  （film 谓词 666 条 head 规则 × pass A/B/C 每规则每目标），瓶颈在枚举
  而非递归深度；谓词界对锚定谓词（film 有基图边）为 0 无剪枝。卡点是
  候选枚举可扩展性，非正确性。

## 回归与 soundness（全过）

Phase-2/3 certified 用例 best_cost 全不变；forward bnb / plain backward /
synth bnb / 退化臂逐字节一致；实例下界开关兼容（两模式复跑一致）。
派生经 EvalAntecedent 门控，零真实 DIVERGENT。

## 证书覆盖范围（更新）

所见候选空间 = pinned 枚举（cap 内）+ 接地单开边（锚定子模式）+
双侧协调（≤64 对/规则/开集）内最优 DAG 证明。截断分两旗：
shape_truncated（开放端点未定/非半连接可接形态）、cap_truncated
（枚举/对数 cap）。协调形态已入空间；film 级稠密谓词规则集上的
穷举枚举性能是遗留卡点。

## 性能与空间数据（供下一轮学习排序 workload）

| goal | 展开数 | wall | 协调候选贡献 |
|---|---|---|---|
| synthc g(a,d) | 3 | <1s | 1（gar_000003 的 (v1,v2) 对） |
| award | 2 | 19s | award 谓词 209 预检，est 剪枝 830 |
| athletes（rules） | 237 | <1s | 0 |
| film | >超时 | ≥900s | —（未完成） |
