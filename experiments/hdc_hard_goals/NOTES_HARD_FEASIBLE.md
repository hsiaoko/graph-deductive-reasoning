# NOTES_HARD_FEASIBLE：资源内可完成的困难 goal 泛化（预注册，2026-11-01）

主 NOTES.md / NOTES_NEW_GOALS.md / NOTES_HARD_GENERALIZATION.md 与其判据
不变。本文件冻结**第二次（也是最后一次）筛选算法**，先于任何新
HDC/numeric 运行。**不依据 HDC/numeric 表现调整**；两个 timeout 候选
（gar_000156 系，~28.6k 展开未 certify）**永久保留为"资源内无法取得
utility 证书"的独立结果**，不删除、不进入 A/B。

## 目标与门槛（写死；不下调）

候选必须同时满足（困难 + 资源可行）：
1. 固定实例、非基图事实；
2. BNB utility（budget=2/cap=64/hybrid/exact/top_k=0）best_cost=2、
   certified_optimal=1、goal_derived=true；
3. utility 展开数 ≥ 1000（不降低）；
4. **utility 能在 2000s 外部 timeout 与安全内存内完成认证**（可行性
   门槛；timeout=资源信息，不判不可证，计入 manifest）。

## 候选与排序（冻结）

数据源不变：`.work_hdcv/hdc_hard_goals/candidates_pool.tsv`
（243,068 个已按结构筛通过的候选；生成过程见
NOTES_HARD_GENERALIZATION.md）。

**新顺序：rule_embeddings 升序（≥1000 起），平键 rule_id 升序 →
rel 升序 → src 升序 → dst 升序。** 从该序列头部开始按序确认——原降
序从最难端开始（池顶 gar_000156 系 ~28.6k 展开无法在 2000s 内
certify）；升序从可行端开始，逐候选验证是否仍满足 ≥1000 展开的困
难门槛。顺序只依赖结构代理与资源可行性，与任何模型结果无关。

## 确认预算（写死，最后一次）

- 同一 utility 设置；外部 timeout 2000s/run、串行；
- **≤6 次 utility 确认运行或 ≤60min 累计，先到先停**；
- 通过条件 = 门槛 1-4 全满足；每次结果（含 timeout）写入
  `candidates_feasible_manifest.tsv`；
- 目标 = 前 2 个全通过者。预算耗尽时：实际几个报几个；1 个则只对这
  1 个做 A/B 并明确样本量 1；0 个则 hard-feasible 维持 pending 并停
  止，**不再开新队列**。

## 训练与 A/B（通过者）

- 排除核查：goal spec 不在任何旧 trace（grep 56 日志 goal 行 + 锚点
  /易例/tasks50）；通过者确无旧 trace 后，HDC 与 numeric **共用同一
  训练池**（原 7 成功 + 49 失败 trace），两臂从同一解析器输出同一批
  (X,y)，重新导出模型（hard_feasible.meta/.bin 与
  hard_feasible_num.meta/.bin），meta 记录 trained_on/train_nodes；
  不改锚点/易例模型。
- A/B：同 binary、同图/1652 规则、budget=2/cap=64/hybrid/exact/
  top_k=0、串行；utility / hdc-value / numeric-value + backward 参照
  （proof_cost 口径）。记录 expansions/wall（含模型开销）/
  proof_cost/certified/chain 等价性/soundness（⊆ 30M 闭包）。正负结
  果全保留。
- 样本量如实：≤2 个 goal，结论保持初步/有限；谓词族隔离样本不足维
  持 N/A。

## 安全（不变）

看门狗 MemAvailable<100GB kill；已用 >400GB 立即人工 kill；串行；
产物 .work_hdcv/hdc_hard_goals/；不重复危险 1652 uncapped chase；
不借 rules512 数字；不 git commit/push/reset；不改 third_party。
