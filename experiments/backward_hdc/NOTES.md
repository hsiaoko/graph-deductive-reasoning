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
