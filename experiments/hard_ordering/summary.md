# hard_ordering：硬场景下学习排序的再检验 —— 最终判定

日期：2026-10-29。判据预注册见 NOTES.md。背景：hdc_value2 在线成功（bnb
臂 -87% 展开）、backward_hdc 的 50-goal A/B 失败（46→45）；本轮在
硬场景（大 OR 扇出/协调形态/此前未命中 goal）重验。

## A 层：主引擎 backward 首解（heuristic vs hdc-value，LOGO/goal）

训练：硬 goal 轨迹（248 个 OR 尝试，仅 2 个成功——硬 goal 大多失败）。
逐 goal LOGO 模型，per-goal 留出运行。

| goal（此前状态） | heuristic hit/spent/s | hdc-value hit/spent/s |
|---|---|---|
| goal41（未命中 r2） | 0/10/0.28 | 0/10/0.45 |
| goal43（未命中 r2） | 0/10/1.38 | 0/10/1.51 |
| goal48（未命中 r2） | 0/10/3.06 | 0/10/25.49 |
| goal49（未命中 r2） | 0/10/4.26 | 0/10/91.26 |
| goal20（232 展开） | 1/1/0.04 | 1/1/0.04 |
| film（协调） | **1**/6/2.11 | **0**/10/2.46 |
| award（协调） | **1**/2/1.83 | **0**/10/41.51 |

命中 3 → 1（丢失 film/award），未救活任何此前未命中 goal，两个 goal
耗时爆炸 8-21×。**判据 1/2 全灭。**

根因（与 50-goal 轮一致且更糟）：硬 goal 轨迹里成功尝试占 2/248——模型
学到"一切失败"，把唯一可用的尝试排到后面。学习排序在缺乏正例的硬场景
上不仅没有信号，还会破坏手工启发式（tier/open_d 证据排序）的正确选择。

## B 层：backward-bnb 证书场景（工程臂 vs 学习臂）

工程臂（惰性枚举，--backward-bnb-enum lazy，证书精确：按规则级下界升序
枚举、incumbent 触发即停）：synthc/athletes 上与 eager 同 cost/证书
（athletes 237→311 展开，同 cost=2——顺序差但正确性不变）；**film 仍未
救活**（30min 超时）：惰性停止只在 incumbent 出现后生效，首个目标的
666 条 head 规则全量枚举本身就是瓶颈——卡点是枚举广度，不是排序。

学习臂在 B 层无效的依据：bnb 候选枚举成本 >> 排序收益；排序不改变
证书（穷举+剪枝下 best_cost 与顺序无关），只影响 incumbent 出现时机——
而 film 的瓶颈在枚举本身。

注：B 层确认 instance 下界是 award 可解的前提（instance: 21s/2 展开
cost=2；predicate 模式 >120s 未决——830 次 est 剪枝+14 次 INF 剪枝是
tractable 的原因）。这为 Phase-3 可选增强补了一个真实获胜场景
（仍是可选开关，不设默认——它改变 expansion 计数路径）。

## 最终判定

**学习排序在硬场景无用且有害（A 层证据确凿），判"不可行"**；判据③正确性
在所有完成 run 上保持（best_cost/certified 一致、派生 ⊆ 闭包、默认逐字节）。
系列总账：HDC 唯一站住的角色 = bnb 分支价值函数（hdc_online 全过）；
OR 尝试排序两轮皆败（样本薄 + 启发式已强 + 硬场景无正例）。
工程上本论的正面产出：backward-bnb 的惰性枚举（证书精确）+ 实例下界的
真实获胜场景（award）。
