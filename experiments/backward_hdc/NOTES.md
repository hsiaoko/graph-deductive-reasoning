# backward_hdc：方向开关 + HDC 价值模型移植到 backward（Phase 1）

日期：2026-10-29。用户决定推理方向做成开关，旧前向完整保留。AND-OR 最优
证书（backward 版 bnb）= Phase 2，本期不做。

## 预注册判据（分析前写死）

任务 2 有效 ⟺ 同时满足：
1. 命中率不降级（Overall 50 goal A/B，LOGO 防泄漏）；
2. 硬 goal 耗时不劣化（mean/p90 不涨）；
3. 命中率提升或硬 goal 耗时降 ≥20% 记为有效提升；
4. soundness：verify 零真实 DIVERGENT；默认配置回归逐字节一致。
任务 1 成功 ⟺ 旧命令（greedy/bnb/verify/synth 全套）逐字节不变 +
冲突组合有明确报错。

## 设计冻结

- --direction forward|backward（默认 forward=旧行为）。--executor backward
  ≡ --direction backward 默认纪律（现有 BackwardExecutor）。
  --direction backward --executor greedy = 退化单路径（首选即终选、不回溯，
  消融臂）。backward+bnb / forward+backward 显式报错。
- HDC-backward 只改 pass-2 的 OR 候选（规则尝试）排序；pinned 匹配、
  半连接求交、计费、派生校验不动。特征与训练/在线同口径（执行时
  attempt 级特征），模型文件与 bnb 版分开（特征清单不同）。

## 结果记录（2026-10-29）

- 任务 1 完成并回归全绿（见 summary）。
- 任务 2：训练 290 尝试 / 21 正例（25/50 goal 有 pass-2）；LOGO 逐 goal
  模型。A/B：46/50 → 45/50（lost 35/44，gained 49——此前未命中 round-2
  之一）；耗时 mean 1.59→3.38s；goal48 3.05s→69.1s。判据①②未过 →
  不可行。分析：样本太薄 + 既有 tier 启发式已很强，模型只在尾部有
  改进空间但噪声伤己。改进方向：失败闸门用法而非全排序。
