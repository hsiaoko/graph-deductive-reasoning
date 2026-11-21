# Learned relevance policy 负增益诊断（FB15k-237 / rules640）

日期：2026-11-21。问题：`--policy learned`（action-goal relevance model）相对
`--policy deterministic` 没有正增益，反而更慢、探索更多事实。本文记录从代码 + 数据
定位的三个根因、确认实验与最终结论。所有数字按 UNIFIED640 口径：36 test goals ×
3 seeds（7/13/42），`execution` 为 execution_cost_ms 均值（秒），`derived` 为派生事实
均值；基线 Deterministic = 2.39 s / 46047.6。

## 被诊断的对象

`core/engine/agent/src/learning/` 的 relevance model（不是旧 HDC-Q）：

- 模型：MLP（tanh 隐层 + sigmoid 输出），输入 `(HDC v_action, HDC v_goal, 手工分数 f)`。
- 标签：`y = γ^{d_C(μ,g)}`，即候选事实 μ 在 goal 证明 DAG 上到 goal 的距离的指数衰减
  （`proof_dataset.cpp` 从 goal 反向 BFS 证明前提得到距离）。正样本 = 证明 DAG 上的事实，
  负样本 = 非证明候选（每 goal ≤ 正样本 × negative_ratio）。
- 打分：`LearnedPolicy::Score = handcrafted_weight·f + relevance_weight·Predict`。
- CLI 默认（`tools/cli/agent_mode.h`）：`learned_handcrafted_weight=0`、
  `learned_relevance_weight=1`。

## 三个根因

### 根因一：训练数据稀缺，模型根本没学会（已实锤）

旧训练只有 10 个 train goals，正样本仅取证明 DAG 上的事实（每 goal 证明仅 ~3 事实），
故 `positive_examples=27 / train_examples=135`，对 ~8321 参数（dim128/hidden32）严重欠定。
`epoch_mse` 8 轮几乎不降（0.2507→0.2408 ≈ 平凡基线），`recall_at_1=0`，输出近常数，
argmax 退化为插入序。瓶颈是 **goal 数太少**，不是 trace 短（trace 共 658,878 条推导，
但绝大多数不在任何 goal 的证明 DAG 上，加长 trace 无用）。

确认：把训练 goal 扩到 135 个**可派生** goal（从 chase 闭包 `closure_edges.tsv` 按
round 2–4 哈希采样，70 个关系，排除 train/test 防泄漏），正样本升到 **6188**、训练样本
**7252**，`epoch_mse` 20 轮 **0.231→0.064**（−72%，单调下降）。模型这次真的学会了。

数据来源坑：`gengoals.tsv` 池全是基图已有事实（trace 全部 `goal_already_in_graph`、
0 推导），不能用作训练 goal；正确来源是只含派生事实的 chase 闭包。

### 根因二：默认权重丢弃手工分数 f（已实锤）

默认 `handcrafted_weight=0` 扔掉了 Deterministic 赖以排序的
`f = α·rel_fam + β·rel_evd − λ·dcost`。把 f 加回（`--learned-handcrafted-weight 1
--learned-relevance-weight 1`，blend11）即从纯 Learned 大幅回收：derived 48448.9→46160.6
（几乎追平 Det），execution 3.83→2.93 s。

### 根因三：标签不是「下一步价值」信号（已实锤，最终结论）

即便模型学会（根因一修复），端到端仍无正增益：

| 配置 | 模型 | 权重 (f / 模型) | execution (s) | derived | hit |
|---|---|---|---|---|---|
| Deterministic | — | — | **2.39** | **46047.6** | 1.0 |
| old Learned | 未训练 | 0 / 1 | 3.83 | 48448.9 | 1.0 |
| blend11 | 未训练 | 1 / 1 | 2.93 | 46160.6 | 1.0 |
| newpure | 已训练 | 0 / 1 | 3.43 | 49409.1 | 1.0 |
| newblend | 已训练 | 1 / 1 | 2.88 | 46125.0 | 1.0 |

- 已训练纯 learned（newpure）仍劣于 Det：derived +7.3%、execution +43%。
- 已训练 blend（newblend）≈ 未训练 blend（blend11）≈ Det（derived 46125 / 46160 / 46047）：
  学会的 proof-proximity 信号叠加在 f 上几乎零增益。

解释：`y=γ^{d_C(μ,g)}` 度量的是「事实在**最终证明**里离 goal 多近」，是证明结构的静态
属性，不是「**当前状态下**选该动作能减少多少剩余搜索」的边际价值；而且它与手工 f 的
`rel_fam=γ^{Σd(q)}`（family 距离）高度重叠，模型本质上在重新学一个 f 已经编码的、带噪声
的版本，自然学不出超越 f 的排序。pairwise accuracy 高 ≠ 更好的下一步排序。

## 修复方向（研究决策，待定）

把学习目标从「回归 proof-DAG 距离」换成「下一步价值」信号，候选：

1. **模仿学习 / learning-to-rank**：以 Deterministic（或更优策略）在每个决策点实际选中
   的候选为正例，学排序而非回归距离——直接对齐「该选谁」。
2. **成本下降价值**：标签 = 选该动作后剩余搜索成本 / 证明成本的下降量（需要反事实 rollout
   或信用分配），对齐「选它能省多少」。
3. 保留 f 为主、模型只学 f 的**残差**（修正 f 的系统性偏差），而非取代 f。

主张 learned policy 优势前须过三关：数据充足（模型学会）→ 不丢 f → 标签是下一步价值。

## 复现脚本

- `run_relevance_scale.py`：从闭包采样可派生 goal、生成 deterministic trace、重训并打印
  报告（`--goals/--hidden/--epochs/--min-round/--max-round`）。
- `run_blend_ablation.py`：36 test goals × 3 seeds 端到端评测，`--handcrafted/--relevance`
  设权重、`--model` 指定模型、`--tag` 区分实验臂。

```bash
# 扩数据重训（确认根因一）
python3 experiments/fixpoint_policy/run_relevance_scale.py --goals 150 --hidden 32 --epochs 20
# 端到端对照（确认根因二/三）
M=.work_hdcv/relevance_scale/model_h32_now/relevance.meta
python3 experiments/fixpoint_policy/run_blend_ablation.py --handcrafted 0 --relevance 1 --tag newpure  --model $M
python3 experiments/fixpoint_policy/run_blend_ablation.py --handcrafted 1 --relevance 1 --tag newblend --model $M
```