# hdc_online：HDC 价值函数在线接入 bnb best-first（A/B）

日期：2026-10-28。前轮（hdc_value2/）：离线重放 HDC 比最佳基线再降
82.5%。本轮在线化。用户已批准接入在线主循环。

## 预注册成功判据（分析前写死）

成功 ⟺ 同时满足：
1. 真实展开数相对原 utility 排序下降 ≥20%（与离线重放方向一致）；
2. 真实 wall time 不劣化（含模型推理开销）；展开降但耗时涨需单独分析；
3. 正确性不变：同一 goal 的 best_cost / certified 标记一致、派生 ⊆
   chase 闭包、默认配置（不开新开关）全部回归逐字节一致。
防泄漏：测试 goal 不进入该次评估模型的训练集（LOGO，逐 goal 一个模型，
训练构成写入 summary）。

## 实现约束（自要求）

- 训练侧复用 analyze2.py 特征/分箱/编码口径，导出可序列化模型
  （meta.json + weights.bin：89 个 int8 基向量 + 4097 个 float64 权重）。
- 推理侧 engine/agent 独立小模块（value_model），仅 --bnb-priority
  hdc-value --value-model PATH 时启用；优先级 c + 预测剩余成本，只改
  排序，line-4 过滤/剪枝/证书语义/派生校验不动。
- 默认关闭，既有行为逐字节一致（synth + athletes bnb chain.tsv 回归）。

## 结果记录（2026-10-28）

- 首次接入只改 EvaluateBranch 的 priority：athletes 仅 2058 vs 2084
  （-1.2%）。诊断：deferred-evaluation 下新分支 key=0 全部抢先评估，
  模型 key 只影响已评估子池；且该模型在 athletes 特征上近常数。
- fork 继承父 key（有前途父分支的孩子先被评估）+ clen 口径修正
  （训练用 fork 时父链长）后：r2c_2 -93%、r2c_4 -93%、r2c_8 -31%，
  athletes 仍持平（模型对该 goal 族无区分度，非机制问题）。
- 正确性全绿：4 goal 的 best_cost/certified/chain.tsv 与 utility 全同；
  soundness: pass；默认关闭两次逐字节回归。
- 推理开销 15-21µs/次，可忽略。峰值 RSS 12.7GB（r2c_2）。看门狗零触发。

## synth 小图补测（2026-10-28）

9 goal（5 个链长 2 + 4 个链长 1），LOGO 训练（33-42 节点/模型）。
A/B：round-1 恒等；round-2 在 ±25% 内抖动（2 胜 2 负 1 平），合计 70→65
（-7%）。正确性全绿（best_cost/certified/chain 一致、soundness pass、
默认逐字节）。判定：小图无区分度，机制正确，收益无法体现。
