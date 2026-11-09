# Fixpoint action-level numeric-Q / HDC-Q（rules640）

日期：2026-11-06。目标是在同一图、rules640、同一 36 个测试 goal 和完整 Chase GT 下，检验 learned cost-to-go 排序及 HDC 表示的收益。

## 方法与特征

`q-hdc` 与 `q-numeric` 只改变 ready action 的顺序；DG 过滤、精确求值、派生、W 复活和终止语义不变。两者复用增量堆，priority = −预测剩余求值次数。

11 个 action 特征（顺序固定）：price、pattern_edges、log_support、relfam、relevd、conf、dcost、total、dp0、wit、premises。它们仅包含规则静态信息和 exact scorer 会随 witness 精确更新的 action 信息，避免在每次选择时对完整 R 重算全局状态。交互项为 price×relevd、log_support×dcost、conf×total、dp0×wit。

HDC 为 4096 维分箱随机特征 + ridge；numeric 为标准化原始特征 + 同一交互项 + ridge。两种模型读取完全相同的训练行。

## 训练隔离与标签

测试集完整保留 `.work_hdcv/fixpoint_rules640/goals.tsv` 的 36 个 goal。训练 goal 从 rules640 闭包排除测试集后选取 12 个，round 1、round 2、round>=3 各 4 个，哈希盐为 `20261106-fixpoint-q-train-v1`，优先谓词多样性。

每个训练 goal 分别运行 deterministic 和 random(seed=7) continuation，共 24 个 trace run。每个实际求值 action 记录一次：y = 从该 action（含）到 goal 的剩余实际求值次数；goal 未推出的 trace 不分配有限标签、不用于回归，单独记录。未观察到的 alternative action 不伪造标签。第一版不使用额外 rollouts，不能把训练描述成已做 alternative rollout。

每个成功 trace 按 remaining cost 排序后等距最多取 256 行，控制 HDC 训练规模；两模型采样完全相同。模型只从训练 goal 导出，测试 36 goal 的任何 trace 都不进入训练。

## 对照与指标

测试每 goal 三臂：Fixpoint-D（exact deterministic、fair=0）、Fixpoint-numeric-Q（fair_interval=1024）、Fixpoint-HDC-Q（fair=1024），共 108 run。Q 两臂公平调度相同，表示是唯一差异。其他参数与 rules640 主实验一致：budget=1e9、cap=100000、goal star order、print=0。

每 run 外部 1000 秒、内部 900 秒、进程树及整机已用内存不超过 256GiB；只有整机已用内存低于 210GiB 才启动训练或评估队列，等待 900s 未解除则保持 pending，不杀其他用户进程。

主指标：wall、完整匹配、ready 插入、action 求值、派生、goal hit、soundness、cap/timeout、模型 calls/infer_ms/avg_us 和内存。报告全部 36 goal 的宏平均、中位数时间之和、实际 run wall 总和，以及 D/Q、numeric/HDC 的配对变化。任何 cap、timeout、未命中都保留；正确性是派生属于 rules640 完整闭包，不是编造准确率。

产物为 `.work_hdcv/fixpoint_q640/`，不覆盖历史结果。

## 最终状态：对照完成，有 cap、无 timeout

12 个训练 goal 与 36 个测试 goal 无重叠。24 个训练 run 中 23 个命中，导出 5,757 个共同采样训练行；`t11/deterministic` 在内部 900 秒上限终止，未用于回归。全部 7,309,499 条训练派生通过完整 Chase 闭包检查。numeric/HDC 的 C++ 与 Python 推理已交叉验证。

首轮 Q 对照暴露 ready 调度 bug：结论已存在的 action 被 admissible 过滤从堆丢弃但滞留在 R，导致 `no_candidates` 提前退出。首轮结果完整保存在 `runs_admissible_bug/` 与 `summary_admissible_bug.*`，不用于性能结论。修复后已知结论走原有零成本 `skip-known` 路径，再按原协议完整重跑 108 run。

修复后三臂均为 36/36 goal 命中、soundness=1、timeout=0，每臂均有 9 次 candidate cap。宏平均 wall / 求值数：D=7.629s / 45,616.1；numeric-Q=7.774s / 43,803.7；HDC-Q=9.540s / 45,672.3。numeric-Q 比 D 少 3.973% 求值，但宏平均 wall 为 0.981x（略慢）；HDC-Q 比 numeric 多 4.266% 求值、wall 为 0.815x。此设置不支持“HDC-Q 优于 numeric 或 D”的结论。完整数据与逐 goal 配对结果见 `.work_hdcv/fixpoint_q640/summary.md` 和 `summary.json`。
