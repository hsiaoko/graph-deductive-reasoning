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
