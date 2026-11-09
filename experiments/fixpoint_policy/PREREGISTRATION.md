# Fixpoint / Chase / HDC-value 最小真实图实验（预注册）

日期：2026-11-05。本文在任何正式测试运行前冻结。正式产物写入
`.work_hdcv/fixpoint_policy/runs_v1`，不覆盖历史实验。

## 1. 两个分开的命题

1. **目标制导命题**：对单个 ground goal，FixpointExecutor 在不改变逻辑答案的
   前提下，比完整 CPU Chase 使用更少派生和更少冷启动时间。
2. **学习排序命题**：在确有分支空间的 BNB 中，HDC cost-to-go value ordering
   比 utility ordering 减少展开数；numeric ridge 是表示对照。

不把 BNB 的 HDC 结果写成 Fixpoint action policy 的结果。rules512 没有 YAML
antecedent，且 Fixpoint 固定 star 顺序并排空每个 ready set；两个 pilot（ground
round-1/round-2 goal）中 det/random 的求值数相同，说明 action 排序在该 workload
上没有减少动作数的机制空间。因此本轮不实现或伪造 Fixpoint-HDC-Q 臂。

## 2. Fixpoint vs Chase

- 图：`/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr`。
- 规则：既有 `.work_hdcv/full_policy_matrix/rules512`，512 条，不重新筛选。
- ground truth：既有 `gt_closure512`，22 轮、349,552 条派生、fixpoint=true、
  无 cap；另跑一次当前二进制的 Chase 并逐字节对账 closure。
- 正例：仅从 GT 的 derived facts 选取 12 个，round=1、round=2、round>=3
  各 4 个。各层内按 SHA256(`20261105|relation|src|dst`) 排序，优先每层关系
  不重复；排除两个 pilot 事实。选择不读取任何 Fixpoint/HDC 性能结果。
- 负例：4 个同谓词重组实体对，必须不在原始 train.txt 或 GT 闭包中。
- 正例臂：exact/deterministic 3 次计时重复；exact/random 使用 seeds
  {7,13,42} 各一次。负例运行 exact/deterministic 一次。
- 参数：budget=1e9、cap=100000、fair_interval=0、print=0。主比较不启用公平
  扰动；任何 cap/timeout/OOM 都使该 run 不能用于完备性声明。
- Chase：threads=1，3 次，max_rounds=1000，max_derivations/matches=1e9，
  no-matches。Fixpoint 为串行；两者主 wall 均采用同一外部看门狗口径，包含
  装载与输出。

指标：goal 判定准确率、派生 soundness（每个派生属于 GT 闭包）、wall、
evaluate_events、派生数、candidate_caps、峰值 RSS。逐 goal 先取中位数，再做
不加权宏平均。Chase 同时报完整闭包 T 与 12-goal 摊销 T/12，禁止混用。

## 3. BNB HDC-value 对照

- 图同上；规则为原始完整 1,652 条，仅做 BNB，不运行其全量 Chase。
- 固定锚点：r2c_4 与 r2c_8；选择它们在运行前写入，不依据本轮结果调整。
- 共同参数：budget=2、cap=64、top_k=0、policy=hybrid、scorer=exact。
- 三臂：utility、hdc-value、numeric-value。HDC 使用 `.work_hdcv/models/<goal>.meta`；
  numeric 使用 `.work_hdcv/hdc_hard_goals/models/<goal>.meta`。两者对应 meta 的
  trained_on 均不含被测 goal（LOGO），训练行数一致。
- 每臂一次，串行。主指标为 expansions、wall、goal hit、proof_cost、
  certified_optimal；派生须属于已保存的 30M Chase 闭包。

这是既有锚点上的机制复现，不是新的困难 goal 泛化试验；不得写成独立测试集上的
泛化证据。若 numeric 更好，结论只能是 learned value ordering 有效而非 HDC
表示独占收益。

## 4. 资源硬上限与止损

- 每个 run 外部 wall-clock **1000 秒**；Fixpoint 另设内部 wall-limit=900 秒。
- 实验进程树总 RSS **256GiB**；同时整机 MemTotal-MemAvailable 也不得超过
  **256GiB**，包含其他用户的占用；达到 256GiB 立即终止本实验进程组。
- `run_bounded.py` 每 0.2 秒检查，超时先 SIGTERM、2 秒后 SIGKILL。
- TIMEOUT/OOM 是实验结果，不算命中、不删除、不用更大预算重跑。系统内存接近
  红线时暂停后续队列。

## 5. 预期不是判定

若 Fixpoint 比冷启动 Chase 快且全部对账通过，可支持“单目标目标制导减少工作”；
若不快于批量摊销 Chase，也必须报告。HDC 只有在相同 BNB 语义下减少展开且保持
证书/答案，才支持“学习排序减少搜索”。任何负结果原样保留。

## 6. 实际止损修订（运行后记录）

本节是执行偏离记录，不改写上述冻结设置，也不把修订后的结果当作完整预注册实验。

- p00 的 deterministic 三次与 random 三次均为内部 `termination=timeout`，每次约
  901.5 秒、未推出 goal；p04 已完成的 deterministic 两次也超时。继续对这些相同
  配置做全部重复只会重复消耗限时，不能提供成功证据。
- 因此停止原队列。`p04/det/rep3` 被外部中断，不使用其不完整输出。已经完整结束的
  重复原样保留；其余尚未运行的正例只探测 det/rep1 和 random/seed7，负例仍只运行
  det/rep1。使用 `run_experiment.py --single-pass` 实现，默认完整重复模式仍保留。
- 修订后的报告标为 `single_pass_probe_incomplete`，列出所有未运行计划。缺跑不能计为
  算法失败，也不能计为成功；限时内未解决与逻辑负例分开报告。
- 资源硬上限不变。进程外部 `completed` 只表示进程正常退出；只有 summary 中的
  `termination=goal` 或 `fixpoint` 才用于 Fixpoint 的完备 goal 判定。
- 超时 run 的部分派生仍逐条与 Chase GT 对账。所有 cap、timeout、OOM 和中断均保留，
  不删除 p00/p04，不用更大的预算重跑。

## 7. 优化后复验（非预注册新实验）

看到旧实现的 profile 和 p04 反例后，修改了实现，因此优化后结果属于探索性复验，
不是第 1--5 节冻结实验的重复，也不用于修饰旧实现的失败。

- 输出：项目内 `.work_hdcv/fixpoint_opt/runs_goal`，与旧 `runs_v1` 完全分开。
- 实现：增量 exact/null 评分、R/W 位置索引、deterministic/hybrid 堆、slot/WaitKey
  索引；`--fixpoint-star-order goal`；递归 SCC goal 禁用会排除中间事实的 pin。
- goal、图、512 规则、budget/cap、线程、重复次数和资源限制均沿用冻结文件；
  恢复完整 det 三次与 random seeds {7,13,42}，不是 single-pass。
- Chase 三次冷启动与 BNB 三臂基线使用 `runs_v1` 的已完成结果；在新 run tree 中
  以 symlink 共享，不把 BNB 结果解释为 Fixpoint-HDC-Q。
- 汇总使用 `aggregate.py --work .work_hdcv/fixpoint_opt/report_goal --runs
  .work_hdcv/fixpoint_opt/runs_goal`。该脚本区分外部 completed 与内部 timeout，
  并检查 goal accuracy、派生 soundness、Chase closure hash、cap、wall 和 RSS。
- p00/p01/p04 的阶段性对账只用于开发定位；最终结论以完整 12 正例 + 4 负例
  矩阵为准，任何漏推/超时都保留。

实际结果见 `.work_hdcv/fixpoint_opt/report_goal/summary.md` 和 `summary.json`。
85/85 个计划 run 已观测，状态 `complete_with_truncation`，validation_failures=0。
Fixpoint-D 正例 12/12，宏平均 3.902s；Chase cold=14.248s，T/12=1.187s。
n01/n02 为 candidate_cap，p10 random 三个 seed 为 timeout，均原样保留。

## 8. Random 采样优化后的单目标复验（运行后记录）

旧 p10 profile 表明约 896s 用于逐次全 R softmax，而不是精确评分或派生。
随后把 Fixpoint exact/null 增量路径改成 Gumbel-max：为存活 action 保存独立
Gumbel 扰动，以扰动后的优先级复用惰性堆。它与 softmax(total/tau) 分布等价，
但随机数流和具体轨迹不与旧实现一致。HDC/Jev 完整评分路径未改。

- 输出：`.work_hdcv/fixpoint_random_opt/runs`，不覆盖 `runs_goal` 或其报告。
- 仅重跑 p10/random 的 seeds {7,13,42}，另以 p04/det 做无随机性的回归。
- 图、规则、goal、budget/cap、wall 和内存限制与第 2 节一致。
- p10 三个新 run 均 `termination=goal`、cap=0，wall 分别为 13.494s、11.260s、
  11.007s；selection 分别为 0.812s、0.735s、0.793s。
- 派生数分别为 28,355、27,059、26,603，全部属于 Chase GT；最大进程树 RSS
  2.580GiB。p04/det 的 chain/closure 与此前正确结果逐字节一致。

这是定位实现瓶颈后的探索性复验，不修改冻结实验结果，也不把其他 goal 的旧
Random 计时与新 p10 混合成完整新矩阵。机制与数字同时记录在
`docs/fixpoint_implementation_plan.md`。
