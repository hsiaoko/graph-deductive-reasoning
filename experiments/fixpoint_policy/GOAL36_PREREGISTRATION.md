# Fixpoint / Chase 36-goal 扩展实验（运行前冻结）

日期：2026-11-06。本实验沿用 `GOAL24_PREREGISTRATION.md` 的图、rules512、ground truth、方法、计数口径和资源限制，只扩大可派生 goal 数量，不加入负例，不改变规则。

## 扩展方式

- 完整保留冻结的 24 个 goal（p00--p23）及其同一二进制、同一参数下的 144 个 Fixpoint run 和 3 个 Chase run。
- 新增 12 个 goal（p24--p35），Chase round=1、round=2、round>=3 各 4 个，使三层各 12 个、总计 36 个。
- 新 goal 排除已有 24 个事实，按 SHA256(`20261106-goal36|relation|src|dst`) 排序，优先覆盖未出现谓词，再按相同哈希补足。选择只读取 GT，不读取新 run 的性能。
- 总矩阵为 219 run：3 Chase + 36×3 deterministic + 36×3 random。旧 run 通过只读 symlink 复用，不复制、不重写；新增实际执行为 72 个 Fixpoint run。

## 输出与预算

输出为 `.work_hdcv/fixpoint_goal36/`。新增 run 继续外部 1000 秒、内部 900 秒、进程树及整机已用内存 256GiB 的硬限制。预计新增约 280 秒，与上一轮约 606 秒合计约 890 秒；估时不作为截断或挑选 goal 的依据。

主指标仍是 wall、物化完整匹配、ready 插入、action 求值、派生、goal hit、soundness、cap/timeout 和内存。逐 goal 中位数后做 36-goal 宏平均，同时报告单目标 cold 加速和 36-goal 批量/Chase 口径。任何 cap、timeout、未命中或闭包外派生均原样保留。

## 运行后结果

219/219 个计划 run 已观测（147 个复用、72 个新增），新增阶段耗时约 333.5 秒；与上一轮两个运行阶段合计约 939.8 秒。最长单 run 30.557 秒，最大进程树 RSS 2.580GiB。

D/R 均为 36/36 命中、timeout=0、soundness=1，宏平均时间 3.778s/4.337s，宏平均完整匹配 251,373.0/265,231.3，cold 加速 4.079x/3.553x。36 个 goal 的中位数时间之和为 136.000s/156.129s，是一次 Chase 的 8.827x/10.133x；批量口径仍不占优。

新 goal p32 的六个 run 各触发一次 candidate cap（D/R 各三次），虽然均推出 goal 且派生属于 GT，仍标为截断命中，不用于完备性声明。其余 210 个 Fixpoint run 无 cap。最终状态为 `complete_with_truncation`，不写成“全矩阵无截断”。

结果见 `.work_hdcv/fixpoint_goal36/summary.md`、`summary.json`、`rows.tsv` 和 `per_goal.tsv`。新增 12 个 goal 改变了宏平均的 goal 混合，因此 24/36 的宏平均数字不应当作同一输入规模下的单调性能曲线。
