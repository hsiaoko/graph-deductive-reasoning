# NOTES_HARD_GENERALIZATION：真正困难 goal 泛化（预注册，2026-11-01）

主 NOTES.md / NOTES_NEW_GOALS.md / 判据不变。本文件冻结**困难候选的
生成与筛选算法**，先于任何 HDC/numeric 运行与 utility 确认运行。
只允许使用 utility/图/DG/闭包结构特征；**禁止**依据任何 HDC 或
numeric 结果挑选、替换或淘汰实例。

## 目标与门槛（写死）

新增 ≥2 个固定实例 goal，全部满足：
1. **未参加过旧训练/测试**：goal spec（rel,src,dst）不出现在任何旧
   trace 的 goal 行（.work_hdcv 的 g*.log / r2c_*.log 共 56 个，
   grep 全量核查），且不在锚点 4、易例 3（g6/g9/g22）、tasks50 任何
   实验 goal 集合中。
2. **proof_cost=2**：非基图事实、BNB budget=2 下 best_cost=2 且
   certified_optimal=1、goal_derived=true。
3. **utility BNB 展开数 ≥ 1000**（困难门槛，不得下调凑数）。
若候选生成/资源使 ≥2 个不可得：保留 hard-generalization = pending，
如实报告；不得用 cost=1 易例顶替。

## 候选生成（确定性，全部留 manifest）

数据源（既有产物，不新跑危险 chase）：
- `.work_hdcv/of_chase30m/closure_edges.tsv`（1652 规则 30M 闭包，
  列：src/rel/dst/round/rule）与其 `summary.yaml`（逐规则
  embeddings/x_satisfied/derived）。
- 规则目录（rule.yaml）用于 head 谓词统计。

步骤（全部确定性）：
1. **proof_cost=2 结构筛**：取闭包中 round == 2 的事实（首派生轮次
   2 ⇒ 存在两步证明且无一步证明 ⇒ budget=2 下 best_cost 恰为 2）。
2. **难度代理筛（不下跑的便宜预筛）**：
   a. 生成规则的 `embeddings ≥ 1000`（匹配空间大）；
   b. 该事实 head 谓词在规则集中有 **≥5 条不同规则**以其为
      consequent（OR 扇出大）；
   c. 排除 head 谓词 ∈ 56 个 trace goal 的谓词（进一步降低旧池污染）。
3. **排除集**：按门槛 1 的全部集合（trace goal 行逐条 grep +
   tasks50.tsv + 锚点/易例 spec）。
4. **确定性排序**：(生成规则 embeddings 降序, rule_id 升序, rel 升序,
   src 升序, dst 升序)。取该序列顺序进入确认流程——**取前 2 个通过
   门槛者**；不人工挑选。
5. **utility 确认**（串行，单 run 外部 timeout 2000s，内部
   wall-limit 不适用——BNB 无 wall-limit 语义）：
   budget=2 / cap=64 / hybrid / exact / top_k=0。通过条件：hit ✓、
   best_cost=2、certified_optimal=1、expansions ≥ 1000。
   确认预算：≤6 个候选的 utility 运行或累计 ≤60 min，先到先停；
   超时的候选记 timeout（资源信息，不判不可证），按序取下一个。
   全部候选与淘汰原因写入 `candidates_manifest.tsv`
   （order/goal_spec/struct_features/utility_result/reason）。

## 训练隔离（若候选通过）

- 新 goal 无任何旧 trace（双重核查：grep goal spec + 检查模型
  trained_on），则 HDC 与 numeric **共用同一训练池**（原 7 成功 +
  49 失败 trace），两臂从同一解析器输出同一批 (X,y)（标签口径不
  变）。用 train_new_goals.py 的独立脚本路径导出
  `hard_pool.meta/.bin`（hdc）与 `hard_pool_num.meta/.bin`
  （numeric），meta 记录 trained_on/train_nodes；**不改锚点/易例
  模型**。
- 谓词族隔离：若新增困难 goal 的 head 谓词与训练池任何成功 trace
  的 head 谓词相同才需要 family-out；否则如实标 N/A（大概率，因
  筛选步骤 2c 已排除 trace 谓词）。

## 运行与判据

同 binary、同图/1652 规则、budget=2/cap=64/hybrid/exact/top_k=0、
串行；utility / hdc-value / numeric-value + backward 参照
（proof_cost 口径按 2026-11-01 更正）。记录
expansions/wall（含模型开销）/proof_cost/certified/chain 等价性/
soundness（⊆ 30M 闭包）。正负结果全保留。新增样本极小，结论标注
初步/有限。

## 安全

看门狗 MemAvailable<100GB kill；已用 >400GB 立即人工 kill；串行；
产物 .work_hdcv/hdc_hard_goals/；不重复危险 1652 uncapped chase；
不借 rules512 数字；不 git commit/push/reset；不改 third_party。
