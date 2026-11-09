# BNB utility / numeric / HDC 六目标消融（运行前冻结）

日期：2026-11-06。目标是比较分支排序，不改变精确推理、bound、剪枝或证书语义。

## Goal 与模型隔离

图与规则为 FB15K-237 全图和原始 1,652 条规则。六个 goal：

- athletes、r2c_2、r2c_4、r2c_8：已有困难锚点，proof_cost=2。
- hard_f1：`/award/award_nominated_work/award_nominations./award/award_nomination/nominated_for(/m/014kq6,/m/01npcx)`。
- hard_f2：`/award/award_nominated_work/award_nominations./award/award_nomination/nominated_for(/m/014kq6,/m/025twgf)`。

hard_f1/f2 是既有 `NOTES_HARD_GENERALIZATION.md` 已冻结的未入训候选，来自 30M Chase 的 round=2 事实，生成规则 embeddings=1070、head 有 41 条规则。其历史 utility 在约 2000 秒外部截断时仍未取得 incumbent，不能写成已完成的负例或最优基线。本轮是首次允许 HDC/numeric 直接处理这两个实例；不得以容易 goal 替代。

锚点 HDC 模型为 `.work_hdcv/models/<goal>.meta`，numeric 为 `.work_hdcv/hdc_hard_goals/models/<goal>.meta`，逐 goal LOGO。新实例共用 `hard_pool.meta` 和 `hard_pool_num.meta`：两者从同一批 33,701 个训练节点导出，成功 trace 为 g0/g6/g9/g22/r2c_2/r2c_4/r2c_8，失败池与旧模型一致，不含 hard_f1/f2 的任何 trace。HDC 为 4096 维随机特征 ridge，numeric 为标准化 11 特征加同四组交互 ridge。

## 运行

每 goal 三臂：utility、numeric-value、hdc-value，共 18 run。共同参数：budget=2、cap=64、top_k=0、policy=hybrid、scorer=exact、print=0。固定 GAR_DEBUG_BNB=1 以保留分支事件，三臂日志开销一致，串行。

每个 run 外部 1000 秒，进程树和整机已用内存不超过 256GiB。BNB 不使用内部 wall-limit；外部 timeout 会留下日志，可能没有 summary.yaml，必须标为未完成而不是不可证。OOM 立即停止队列，不调大限制重跑。

## 指标与判定

- Anytime：首个 goal 的展开数和 executor-local wall（新插桩，不改变搜索）；日志允许时保留 timeout 前的部分信息。
- Optimum：最终展开数、外部 wall、proof_cost、certified_optimal、hit、终止状态。
- 开销：value-model calls/infer_ms/avg_us；训练耗时另记。
- 正确性：所有已写出的派生 ⊆ 30M Chase 闭包；完成臂要求 proof_cost=2 且 certified_optimal=true。替代等 cost 链合法，不要求链逐字节一致。
- 加速：utility/HDC、utility/numeric、numeric/HDC 的 wall 与展开比；只有双方同口径完成才给精确比。utility timeout 而学习臂完成时只给 `1000/learned_wall` 的保守下界，不伪造精确加速。
- 汇总同时保留逐 goal 行、可配对完成的集合与全部六 goal 的覆盖情况。任何 timeout/cap/未命中原样保留，不能只用获胜子集声称普遍优势。

输出 `.work_hdcv/hdc_ablation6/`，不覆盖历史模型或实验。

## 运行中断与已完成结果（2026-11-06）

观测 13/18 run；r2c_2/utility 为 1002s 外部 timeout，r2c_4/utility 和 hard_f1/utility 为整机内存 `oom_system`。两次 OOM 后均停止队列，等待 210GiB 内存门槛 900s 未解除，剩余 hard_f1 numeric/HDC 和 hard_f2 三臂共 5 run 暂停，不再自动重试。没有终止其他用户的进程。

| goal | utility wall / 展开 | HDC wall / 展开 | numeric wall / 展开 |
|---|---|---|---|
| athletes | 177.680s / 2084 | 163.555s / 2058 | 29.340s / 353 |
| r2c_2 | timeout | 109.383s / 1326 | 99.489s / 1254 |
| r2c_4 | OOM | 32.166s / 897 | 25.044s / 710 |
| r2c_8 | 2.949s / 68 | 2.459s / 47 | 4.426s / 120 |

所有完成的 10 个 run 均 proof_cost=2、certified_optimal=true；已写出的派生全部属于 30M Chase 闭包，soundness failures=0。HDC 对 utility 的精确 wall 加速为 athletes 1.086x、r2c_8 1.199x；r2c_2 因 utility timeout 只能给保守下界 >9.142x。numeric 在 athletes/r2c_2/r2c_4 上优于 HDC，HDC 在 r2c_8 上优于 numeric。四个锚点的学习臂合计展开为 HDC 4328、numeric 2437，numeric 少 43.7%。

因此当前证据支持“价值函数学习可减少搜索，HDC 在部分 goal 有效”，不支持“HDC 表示始终优于数值 ridge”。两个新增困难 goal 的泛化结论仍为未验证，不能用四个锚点代替。完整状态见 `.work_hdcv/hdc_ablation6/summary.md` 和 `summary.json`。
