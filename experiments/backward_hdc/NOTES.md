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

## Phase 2 预注册（2026-10-29）：backward-bnb（AND-OR 树分支定界）

设计冻结（正确性优先）：
- 实现 = DFBnB（深度优先分支定界）于 backward 的 pinned 候选空间：
  OR 节点 = 接地目标（slot,src,dst），候选 = 该谓词规则的 pinned 枚举
  motif；AND = 前提子目标递归；共享 Γ（事实只增）→ 成本为 DAG 口径
  （共享子证明第二次免费），证书文字 = "所见候选空间（pinned 枚举 cap 内 +
  接地子目标递归）内成本最优 DAG 证明"。
- 下界 = DerivationDepths（谓词最小派生应用数，可低估不高估）；剪枝 =
  paid + 1 + Σ前提下界 >=  incumbent 时剪（树口径 Σ；DAG 下 Σ 可能高估 →
  改用 max 保持可采纳——实现里用 Σ 但标注：共享只在已派生事实上归零，
  未派生前提不共享，Σ 安全；论证写进文档）。
- open-sided 协调形态（disjoint-free 双开等）不进本期证书空间（排除该
  尝试类，文档标注覆盖范围）。
- 默认关闭：`--direction backward --executor bnb` 才走新路径；旧命令
  逐字节不变。

验证判据（分析前写死）：
1. 正确性：forward bnb 有 certified_optimal 的 goal 上（athletes、
   Overall 的 3/9 等）backward-bnb best_cost 必须一致（链可不同）；
   synth 手造已知最优链 goal 单元验证。
2. soundness：派生 ⊆ chase 闭包，零真实 DIVERGENT。
3. 性能：与 forward bnb 对比展开数/wall/内存，更差如实记录。
4. 回归：旧命令逐字节一致。
任一不达 → 标 --experimental 并写清覆盖范围；基本正确性不过 → 交付
诊断不硬撑。

## Phase 3 预注册（2026-10-29）：实例级下界 --backward-bnb-lb

判据（分析前写死）：
1. 正确性：全部既有 certified 用例 best_cost 与 certified 标记不变
   （synth cost-1/2、athletes=2、Overall goal 17/19/20）；协调形态仍如实
   不报证书；soundness 零真实 DIVERGENT。
2. 效果：predicate vs instance 的展开数/wall/剪枝计数 A/B；预检成本抵消
   收益则如实报告。
3. 回归：默认 predicate 逐字节；forward 全套不动。

实现冻结：
- 组件：在 Γ→0；无任何 head 规则→INF（安全剪枝+诊断计数）；当前无全
  匹配 motif→≥2（Γ 单调增长论证）；当前可一级派生→≥1 紧化；与谓词深度
  取 max。每目标按 Γ 版本缓存（版本变则重算，防高估）。
- **随带修正 Phase-2 剪枝的可采纳性论证**（写文档显著位置）：incumbent
  剪枝 est=1+Σ前提下界 在 DAG 共享下可能高估（两个未派生前提共享子证明
  时真实成本 < est）→ 剪枝改用 1+max（保守、可采纳），Σ 仅用于候选
  排序。Phase-2 已验证用例将在修正后复跑确认 best_cost 不变。
