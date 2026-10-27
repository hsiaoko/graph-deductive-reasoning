# HDC 换编码做经验泛化 —— 门 1 验证 NOTES

日期：2026-10-27。前置：experiments/hdc_fair（随机编码无信号：4M 对采样，
结局一致率 0.509 ≈ 基线 0.500）。

## 预注册判定标准（写死于数据分析之前，不许事后移动）

门 1 通过 ⟺ 同时满足：
1. 高相似带（编码 A 或 B）结局一致率相对基线的 lift > 5 个百分点
   （leave-one-goal-out 划分下）；
2. 最近邻（1-NN，余弦）结局预测显著优于平庸基线（谓词→历史成功率计数表）。
否则判定无信号，建议放弃该方向。

## 数据

- 300 个 goal：150 正（闭包 round≤2 边，229 个关系分层采样）+ 150 负
  （同关系随机实体对，不在闭包正例集中），budget=10，backward executor，
  GAR_DEBUG_BACKWARD=1 全轨迹（工具 /tmp/hdc_g1/back_multi.cpp——多 goal
  单进程：规则加载一次、图按 goal 重载并 hold-out 目标边；推理代码零改动）。
- 子目标实例 = 轨迹中每条 try 目标；结局标注：出现在该 goal 的派生 journal
  中 → 成功；显式 exit->0 且不在基图 → 失败；其余（环/memo 早退）弃用。
  基图边成员判定用 immutable_csr 全量边转储（dump_csr 工具）。
- 样本量与正负比例：跑完后如实记录。

## 编码

- 随机（对照，上轮结论）：符号全部 splitmix 随机向量。
- 编码 A（schema 层）：谓词向量 = bundle(特征符号)，特征 = 作为 head/body
  的规则数分桶、实例数分桶、出/入度分桶等（goal 无关，防泄漏）；子目标 =
  bind(v_pred, bundle(v_src, ρ v_dst))，常量仍随机向量——测纯 schema 泛化
  （同谓词不同常量）。
- 编码 B（实体邻域层）：v(e) = bundle(bind(v_rel, v_neighbor))（基图全量
  邻域，不含 hold-out 影响），子目标组合同 A。
- 平庸基线：谓词→训练集成功率计数表（按谓词预测 P(success)）。

## 指标

相似度分带结局一致率 lift 曲线；1-NN 预测准确率 vs 计数表；全部按
leave-one-goal-out（同 goal 的子目标对/近邻不算）。
