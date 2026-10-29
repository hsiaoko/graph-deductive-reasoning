# exp-1 Overall：5 方法 × 50 goal 主对照（预注册）

日期：2026-10-29。问题：策略制导推理是否真比全量 chase 快（论文主表）。

## 预注册汇总表格式与判定口径

主表（每方法一行）：命中率 hit%、平均/中位/p90 time-to-goal（agent 主循环；
端到端单列）、平均派生事实数、峰值内存。bnb 两臂加：展开数、best_cost、
certified 一致性。分层明细按 bstar_round（round-1 / round-2）。
判定口径（不与别轮混用）：agent 方法 hit = goal_derived 且 soundness 通过；
chase hit = goal ∈ 截断闭包。加速比 = chase 端到端时间 / agent 端到端时间
（agent 含规则加载+DG 的固定开销则单独注明）。

## Setting（用户确认版）

- 数据 fb15k-237 immutable_csr；goal 50 个 = ablation 20（18×r1 + 2×r2）
  + 新采样 30（分层补 round-1/round-2 至各 25；从 rules_g3 shared-holdout
  闭包的可派生边，seed 固定）。bstar_round 记录进明细 tsv。
- 方法：①全量 chase（CPU 8 线程，2 轮，100k 匹配截断，30M 上限，
  rules_g3 81,683，一次）；②backward（rules_g3，budget=10，cap=8）；
  ③greedy deterministic+exact（同配置）；④bnb+utility（rules 子集 3023，
  budget=2，cap=64，top_k=0，30min/goal 超时）；⑤bnb+hdc-value（同 ④，
  共享模型——训练池为 .work_hdcv 的 7 成功+40 失败轨迹，50 goal 均不在
  训练池：采集前逐一核对）。
- bnb 两臂只覆盖 rules 子集 2 轮闭包（.baseline/fb_deduce_cpu，648,595
  事实）内且非基图的 goal，其余 N/A。
- 硬控制：soundness（agent 派生 ⊆ 闭包，零 DIVERGENT）、看门狗
  （MemAvailable<100GB kill）、chase 串行、bnb 4 路并发上限、timeout 1800s、
  seed 固定。
