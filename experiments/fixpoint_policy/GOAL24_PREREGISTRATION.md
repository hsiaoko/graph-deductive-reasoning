# Fixpoint / Chase 24-goal 扩展实验（运行前冻结）

日期：2026-11-06。本实验只评估可派生 goal 的命中成本，不评估不可派生目标的拒绝能力。

## 输入与对照

- 图：`/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr`。
- 规则：`.work_hdcv/full_policy_matrix/rules512`，512 条，不重新筛选。
- ground truth：`.work_hdcv/full_policy_matrix/gt_closure512`，22 轮、349,552 条派生、fixpoint=true。
- Chase 输入为图和规则；Fixpoint 输入为图、规则和一个 ground goal。goal 是否可派生的标签只用于选样和验收，不传给推理算法。

## Goal 冻结

保留已有 p00--p11 十二个锚点，新增十二个，总计 24 个正例；不加入负例。三层为 Chase round=1、round=2、round>=3，各含原有四个和新增四个。

新 goal 只读取 GT 闭包，不读取任何新实现的运行结果。排除已有 12 个事实，层内按 SHA256(`20261106|relation|src|dst`) 排序，优先选取锚点和本层已选 goal 尚未覆盖的谓词，再按同一哈希顺序补足。新 goal 依次编号 p12--p23。冻结文件为 `.work_hdcv/fixpoint_goal24/goals.tsv`。

## 方法与重复

- Chase cold：三次，threads=1，max_rounds=1000，derivation/match cap=1e9，不保存 matches。
- Fixpoint-D：exact scorer、deterministic policy、goal star order，每个 goal 三次。
- Fixpoint-R：exact scorer、Gumbel-max random policy、goal star order，每个 goal 使用 policy seeds {7,13,42}。
- Fixpoint 共同参数：budget=1e9、candidate cap=100000、fair_interval=0、print=0。
- 共 147 个 run：3 Chase + 72 deterministic + 72 random。串行运行。

## 时间与分支口径

主指标是外部 wall time 和物化工作量，不使用“准确率”混淆正例命中与负例拒绝。

- `embeddings_total`：Chase 所有轮次、所有规则的 label-valid 完整匹配总数；既有 per-rule embeddings 仍是最后一轮值。
- `fx_match_embeddings`：Fixpoint 实际物化的 label-valid 完整匹配数，包括 delta/部分匹配完成后形成的完整匹配，不包含 matcher 内部回溯或尚未完成的 partial embeddings。
- `fx_ready_insertions`：进入 R 的次数，包含 revival 重新入队。
- `fx_evaluate_events`：实际选择并求值的 action 数；不等于匹配数。
- `derived`：实际新派生事实数，不能冒充分支数。
- BNB expansions 不属于本实验，不与上述计数混用。

Chase 与 Fixpoint 的匹配计数都描述物化的完整候选，但语义一个是全规则全轮、一个是 goal-cone 加增量唤醒；比较的是实际工作量，不声称二者具有相同搜索树。

## 验收与报告

- 正例命中：termination=goal 且 goal_derived=true；timeout、budget、未推出 goal 均保留为失败/未解决。
- 每个 Fixpoint run 的全部派生必须属于 GT 闭包；cap 单独报告，不用 cap 结果声称完备性。
- 当前三次 Chase closure 必须与冻结 GT 逐字节一致，fixpoint=true。
- 逐 goal 对重复运行取中位数，再做 24 goal 等权宏平均；同时保留 run-level 行。
- 报告独立单目标冷启动口径，以及 24 目标总 Fixpoint 时间与一次 Chase 的批量口径；Chase T/24 只作摊销参考。

## 硬限制

每个 run 外部 1000 秒；Fixpoint 内部 900 秒。实验进程树和整机已用内存均不得超过 256GiB，达到限制由 `run_bounded.py` 终止进程组。任何 timeout/OOM 都作为结果保留，不调大限制重跑。输出写入 `.work_hdcv/fixpoint_goal24/runs`，不覆盖历史实验。

## 运行后结果

147/147 个计划 run 已观测，未修改上述 goal 或参数。Chase 三次分别耗时 15.408s、15.676s、15.050s，闭包均与冻结 GT 逐字节一致。Fixpoint-D/R 均为 24/24 命中、cap=0、timeout=0、soundness=1；宏平均时间分别为 3.630s、3.916s，宏平均物化完整匹配分别为 321,659.4、334,441.9。

相对冷启动 Chase，D/R 加速 4.244x/3.935x，完整匹配减少 99.895%/99.891%。24 目标中位数时间之和为 87.128s/93.983s，是一次 Chase 的 5.655x/6.100x，故不声称批量总时间占优。两个运行阶段合计约 606s；最长单 run 20.865s，最大进程树 RSS 2.580GiB。

最终产物：`summary.md`、`summary.json`、`rows.tsv`、`per_goal.tsv`。运行与汇总脚本分别为 `run_goal24.py`、`aggregate_goal24.py`；机制解释同步到 `docs/fixpoint_implementation_plan.md`。
