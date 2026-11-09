# Unified GoalChase / Deterministic / Random / Learned（rules640）

日期：2026-11-08。目的：消除旧报告中单次/三重复、fair interval 和执行批次混用的问题。本文件在运行前冻结统一协议；旧实验保留，但不与新表混合计算。

## 输入与共同执行参数

- 图：`/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr`。
- 规则：`.work_hdcv/fixpoint_rules640/rules640`，640 条。
- 测试 goal：`.work_hdcv/fixpoint_rules640/goals.tsv` 的同一 36 个 goal。
- Ground truth：`.work_hdcv/fixpoint_rules640/runs/chase/rep1` 的完整 Chase 闭包。
- 所有方法均为 `--executor fixpoint --fixpoint-star-order goal`。
- 共同参数：budget=1,000,000,000、candidate cap=100,000、fair interval=0、print=0。
- 所有方法均使用 policy seeds `{7,13,42}`。Deterministic、GoalChase 和 Learned 不消费 policy 随机数，因此这三 run 是相同逻辑的计时重复；Random 的三个 seed 产生不同采样轨迹。
- 每 run 外部 1000 秒、内部 900 秒；进程树及整机已用内存上限均为 256 GiB。串行运行，只有整机已用内存低于 210 GiB 才启动，不终止其他用户进程。

## 五个方法臂

只有 action 选择机制及其实现所需的 scorer/model 配置不同：

1. GoalChase：`--policy deterministic --scorer null`。
2. Deterministic：`--policy deterministic --scorer exact`。
3. Random：`--policy random --scorer exact`；在线 Gumbel-max 与 softmax(total/tau) 分布等价。
4. Learned numeric-Q：`--policy q-numeric --scorer exact --value-model .work_hdcv/fixpoint_q640/models/q_numeric.meta`。
5. Learned HDC-Q：`--policy q-hdc --scorer exact --value-model .work_hdcv/fixpoint_q640/models/q_hdc.meta --hdc-value-backend original`。为与已有 HDC 模型开销分析一致，本矩阵固定原始后端；bit-LUT 是另一个计算内核实验。

所有臂共享 DG goal-relevance 过滤、候选生成、精确前提求值、事实派生、挂起/复活和 goal 提前停止。不重新选择规则、goal 或训练数据，不根据新结果替换模型。

## 运行规模与聚合

- 五臂 × 36 goals × 3 seeds = 540 个 goal-directed run。
- Chase 是完整闭包，不属于逐 goal 五臂矩阵；沿用既有三次 cold 结果，仅作非等价工作量参照。
- 每 goal、每方法先对 seeds `{7,13,42}` 的指标取中位数，再对 36 个 goal 等权宏平均。
- 主指标：外部 wall、内部 wall、`fx_select_events` 选择分支、`fx_ready_insertions`、`fx_match_embeddings`、`fx_evaluate_events`、派生、goal hit、soundness、cap、timeout 和 RSS。
- 选择分支包含零成本跳过已知结论；真实求值单列，二者不得混用。
- 表中的 36-goal 代表时间是逐 goal 中位数之和；实际 108-run 总 wall 另行报告。
- 所有派生必须属于完整 Chase 闭包。cap、timeout、未命中和 OOM 原样保留，不调高限制重跑。

## 产物

统一输出到 `.work_hdcv/fixpoint_unified640/`，不覆盖旧实验。运行与汇总脚本为 `experiments/fixpoint_policy/run_unified640.py`。
