# 总体试验计划：完整答案推理与困难证明搜索分开评估

日期：2026-11-01。本计划修正此前只围绕自由变量完整答案任务评估 HDC
scorer 的覆盖缺口。已完成的实验不 retroactively 改判据；新增实验先冻结
goal、训练隔离和对照，再运行。

## 1. 要回答的两个问题

### 实验 A：agent 是否比全量 chase 更快且答案完整？

任务：goal 含自由变量，求完整答案外延。沿用已完成的 rules512 矩阵，不重跑。

主对照：chase、backward-deterministic、forward-null-deterministic、
forward-exact-deterministic；其余 policy/scorer 放补充结果。

指标：执行器 wall、最终派生数及派生缩减率、all-answer recall、
derived-only recall、precision。chase 同时报告完整闭包时间与多 goal 摊销
时间，两者不得混用。

该实验的结论边界：它评估的是完整答案计算和 HDC scorer，不用于否定
BNB 的 HDC 价值函数。

### 实验 B：HDC 价值函数能否减少困难证明搜索？

任务：固定实例 goal，寻找并认证最优证明链。主比较必须是同一 BNB 搜索器的
utility 与 HDC-value 在线 A/B，而不是拿 HDC scorer 的结果代替。

固定设置：fb15k-237、原规则集 1,652 条、budget=2、cap=64、
hybrid/exact、top_k=0。旧日志中的 3,023 是 Dependency Graph 弧数，
不是规则数。

主对照：

1. BNB-utility：priority = cost + minprice。
2. BNB-HDC-value：priority = cost + HDC/ridge 预测剩余成本。
3. BNB-numeric-value：使用相同 11 个原始特征及相同交互项的数值 ridge，
   训练特征标准化只由训练集拟合；用于检验收益是否专属于 HDC 表示。
4. backward-deterministic：目标制导的非 BNB 参照。
5. chase：补跑同图、同规则 workload 的总体参照；不得借用 rules512 的
   13.58s。若不能在安全资源内完成，明确标 N/A，不用截断结果冒充 GT。

主 A/B 固定 hybrid/exact。policy 敏感性作为附加矩阵，不与主 A/B 的
控制变量混在一起。

## 2. Goal 设计：保留困难代表，也保留负面对照

复核锚点：r2c_2、r2c_4、r2c_8、athletes。四个都保留，尤其不能只保留
HDC 大胜的 r2c_2/r2c_4。

新增 goal 必须与它们任务类型相同：常量绑定的固定实例证明，而非自由变量
全外延查询。候选 goal 在运行 HDC 前，依据原始 BNB 的可观测搜索特征冻结：

- 非平凡证明链，目标链长为 2；
- 有明显 OR 扇出和失败分支；
- utility BNB 展开数达到预先记录的困难门槛；
- 同时保留简单 goal 和 athletes 类低收益 goal 作为负面对照。

不得依据 HDC 是否获胜筛选、删除或替换 goal。新增测试 goal 不得出现在
其模型的任何成功或失败训练日志中。

## 3. 训练隔离

1. 主评估继续使用 LOGO：测试 goal 的全部轨迹从该模型训练集中剔除。
2. 新增 goal 若与训练 goal 共享结论谓词，增加 leave-one-predicate-family-out
   分组测试，检验是否只是记住关系族。
3. 训练配置、图、规则、budget、cap 与在线评估一致；记录每个模型的
   trained_on、train_nodes、HDC seed 和特征版本。
4. HDC-value 只改变开放分支排序，不改变精确匹配、派生校验、逻辑剪枝和
   证书语义。在线 wall 包含编码与模型推理开销。

## 4. 指标与判据

实验 B 主表 X 轴为 baseline，Y 分别为：

- 执行器 wall time；
- BNB 分支展开数（chase/backward 无此概念时标 N/A，不填 0）；
- goal-hit rate、best_cost、certified/certified_no_chain。

固定实例单链证明不与完整答案 recall 混表。所有方法另行核验派生
soundness；同一 goal 的证明任务下不得通过降低正确性换速度。

HDC 主 A/B 的最低成功判据沿用原预注册：相对 utility，展开数下降至少
20%，端到端 wall 不劣化，best_cost、证书和推导链一致。锚点复核与新 goal
泛化分开报告；数值 ridge 与 HDC 的差异单独报告，不预设 HDC 必然更优。

## 5. 论文图表与结论边界

图 A（已完成数据）：完整答案任务，时间 / 派生缩减 / recall 三联图。

图 B（待补实验）：困难证明任务，时间 / BNB 展开数 / goal-hit 与证书图。
两个图分别支持“选择性 agent 推理的整体优势”和“HDC 价值排序的搜索加速”，
不得相互代替。

安全约束不变：串行计时；MemAvailable<100GB 时看门狗终止相关进程；
全机已用内存超过 400GB 立即终止实验；重要产物放 .work_hdcv。

## 6. 状态（2026-11-01 更新）

- 实验 A：**完成**（rules512，110/110，见 experiments/full_policy_matrix/
  summary512.md）。
- 实验 B：**锚点复核完成 + 易例对照完成；困难泛化 PENDING**
  （experiments/hdc_hard_goals/summary.md）：
  HDC-value 合计展开 -87.2% 复现；numeric-value 合计 **-92.8%**——
  value 收益不专属于 HDC 表示（athletes 上 numeric -83.1% vs HDC
  -1.2%；仅 r2c_8 HDC 胜出）。
  易例对照（g9/g22/g6，proof_cost=1）：三臂展开逐数一致（无害无益），
  收益边界 = 需要足够大分支空间；**不计入困难泛化证据**。
  困难泛化（proof_cost=2 且 utility ≥1000 展开）：两轮筛选均失败——
  第一轮降序池顶 2 候选（gar_000156 系）各 ~28.6k 展开 timeout；第二
  轮升序（NOTES_HARD_FEASIBLE，可行性门槛）2 候选（gar_000202 系）各
  ~74.4k/74.9k 展开 timeout 且 best_cost=-1（goal 未到达）。确认预算
  耗尽 → **PENDING/未验证**（cost-2 证书需穷尽数万 cost-1 前沿分支，
  超出 2000s 包络；机制与留痕见 summary.md C 节与两个 manifest）。
  （2026-11-04 断点恢复整理：独立复核 f1/f2 日志首末行与冻结队列前两
  行一致；候选生成固化为 make_hard_candidates.py 并复跑逐字节一致。）
  backward 成本口径已更正：proof_cost = chain.tsv 末行 cum_cost
  （4/4=2 与 BNB 最优一致）；summary total_cost = search_spent，不再
  混用。chase 按 §2.5 标 N/A（资源包络证据充分）。
