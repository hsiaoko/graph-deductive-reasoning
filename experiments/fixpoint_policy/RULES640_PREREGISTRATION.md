# rules640 / 36-goal 独立实验（运行前冻结）

日期：2026-11-06。本实验只改变规则数量，图和 36 个 goal 不变，不沿用 rules512 的闭包或 Fixpoint run。

## 规则冻结

- 基础：`.work_hdcv/full_policy_matrix/rules512` 的全部 512 条规则。
- 候选全集：`/ssd_data/zhuxk/graphs/KG/fb15k-237/rules` 的 1,652 条规则。
- 从候选全集排除已有 512 条，按 SHA256(`20261106-rules640|rule_id`) 排序，取前 128 条，形成 640 条规则。
- 规则目录只包含指向原始 GAR 目录的只读 symlink；保存新增 ID、完整 ID 哈希和二进制哈希。不得根据运行时间、派生数量或 goal 命中情况替换新增规则。

## 输入与 ground truth

图仍为 FB15K-237；goal 完整复用 `.work_hdcv/fixpoint_goal36/goals.tsv`，round 分层仍表示 rules512 下的首次派生轮次，用于保持样本一致。

输出 `.work_hdcv/fixpoint_rules640/`。先运行三次 CPU Chase（threads=1、max_rounds=1000、match/derivation cap=1e9、不保存 matches）。第一次闭包作为新 GT，后两次必须逐字节一致。只有三次均 `fixpoint=true`、无 cap、包含全部 36 个旧 goal 时才启动 Fixpoint；否则停止并报告。

## Fixpoint 与指标

每个 goal 运行 deterministic 三次和 random seeds {7,13,42}，共 216 run；加三次 Chase，总计 219 run。参数与 goal36 相同：exact scorer、goal star order、budget=1e9、candidate cap=100000、fair_interval=0、print=0。

每个 run 外部 1000 秒、内部 900 秒，进程树及整机已用内存不超过 256GiB。串行执行。任何 cap、timeout、OOM、未命中均保留，不调大限制重跑。

报告 wall、物化完整匹配、ready 插入、action 求值、派生、goal hit、soundness、cap/timeout 和内存；逐 goal 中位数后做 36-goal 宏平均，同时报告所有重复 run 的实际 wall 总和及批量/Chase 口径。rules512 与 rules640 使用同一 36 个 goal，可比较同一目标的绝对变化，但规则集不同，不能复用旧 GT 或将新旧 run 混作同一实验。

## 运行后结果

219/219 个计划 run 已观测，状态 `complete_with_truncation`。三次 Chase 均到达不动点且闭包逐字节一致，包含全部旧 goal；rules512 闭包是 rules640 闭包的子集（349,552/5,019,342，0 条缺失）。

| 方法 | rules512 宏平均 wall | rules640 宏平均 wall | 时间倍数 | rules640 宏平均完整匹配 | rules640 宏平均求值/派生 |
|---|---:|---:|---:|---:|---:|
| Fixpoint-D | 3.778s | 11.104s | 2.939x | 455,325.2 | 45,616.1 |
| Fixpoint-R | 4.337s | 11.698s | 2.697x | 472,083.4 | 46,027.8 |

Chase 中位时间从 15.408s 增至 742.462s（48.187x），全轮匹配从 306,101,189 增至 13,041,165,813（42.604x），派生从 349,552 增至 5,019,342（14.359x）。D/R 在 rules640 上均为 36/36 命中、timeout=0、soundness=1，cold 加速为 66.863x/63.467x。

36 个 goal 的 D/R 中位数时间之和为 399.750s/421.142s，低于一次 Chase 742.462s；时间比为 0.538x/0.567x，即批量口径分别加速 1.857x/1.763x。与 rules512 的批量慢 8.827x/10.133x 相比，这个冻结的更大规则集显示了目标制导避开无关全量计算的价值；不能据此声称所有规则扩展都存在相同拐点。

所有 216 个 Fixpoint run 均命中且派生属于新 GT，但 p11/p21/p32/p34 共 24 个 run 出现 cap，总 cap 次数 54（D/R 各 27）。这些截断命中不用于完备性声明。最长 Fixpoint run 200.459s，最长 Chase run 835.628s，均低于 1000s；最大 Fixpoint RSS 5.412GiB。全部 219 个进程的 wall 总和为 4726.016s，两个后台阶段合计约 4737.8s；单 run 达标不代表整批低于 1000s。

完整报告位于 `.work_hdcv/fixpoint_rules640/summary.md`，逐 run/goal 数据位于同目录。
