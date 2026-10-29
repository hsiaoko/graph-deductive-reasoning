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
