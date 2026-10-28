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
