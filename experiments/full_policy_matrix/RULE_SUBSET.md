# 预注册补充：规则子集选择（用户授权"少用点规则"）

日期：2026-10-31。原 NOTES.md 保留有效；本文档写死子集选择算法与可行
性阶梯，先于任何规则生成与 GT 运行。**选择只利用规则内容、DG、挖掘
quality、goal 名称；禁止利用任何 forward/backward/HDC 实验表现。**

## 可行性阶梯（只此一次降级）

1. **首选 64 条**（rules64）。完整 chase GT 门槛不变：无有效
   cap 的真实 fixpoint（max-rounds 1000、max-derivations/matches 1e9、
   print 0、主计时 threads=1、threads=8 仅附加参考），1800s + 400GB
   红线 + 看门狗。
2. 若 64 不可行 → **同一最终有序列表的前 32 条**（rules32，嵌套）。
3. 若 32 也不可行 → 停止并如实报告 blocked，不再降级。

## 确定性选择算法（写死）

记号：规则 r 的 head(r) = consequent 谓词名；body(r) = pattern 边 label
∪ antecedent 谓词名。DG 弧：body 谓词 → head 谓词（每规则每条 body
谓词一条弧）。reach(r, g) ⟺ DG 中 head(r) 可达 goal 谓词 g（沿
body→head 弧，距离有限）。

排序键（所有池通用）：confidence 降序 → support 降序 → rule_id 升序
（全确定性，无随机）。

步骤：
1. 对 goal k（k=1..5，题目顺序）：直接 head 池 H_k = {r : head(r) ==
   goal_k 谓词}；取 top-4（不足全取）。**目标覆盖：每 goal 至少 1 条
   直接 head 规则**（若 H_k 为空，该 goal 记 no-head，manifest 注明）。
2. 已选集 S = ∪_k head_picks(k)（|S| ≤ 20；5 个 goal 谓词两两不同，
   head 池互不相交）。
3. 祖先池 A_k = {r : reach(r, goal_k)} \ S，各自按排序键排序。
4. round-robin：按 k=1,2,3,4,5,1,2,... 顺序，每轮从 A_k 头部取一条
   （跳过已在 S 的），加入 S，直到 |S|=64 或全部池耗尽（耗尽则不足
   64，manifest 注明实际 N）。
5. **最终有序列表 L = 选取顺序**（head picks 按 goal 序在前，
   round-robin 序列在后）。rules64 = L；**rules32 = L 的前 32 条**
   （嵌套）。
6. 嵌套覆盖保护：因 |S_head| ≤ 20 ≤ 32 且 L 前 20 即全部 head picks，
   前 32 必含每 goal 的全部 head picks → 32 子集自动保留每 goal 直接
   head 覆盖。若未来改动使 head picks > 32，则改为：先保留每 goal 首
   条 head 规则（共 ≤5 条），再按 L 顺序填充至 32（跳过已保留者）。

产出：`rules64/`（symlink 到原规则目录，RuleLoader 直接可读）+
`manifest.tsv`（order / rule_id / head / selected_reason /
confidence / support）+ 本算法脚本文本（experiments/full_policy_matrix/
select_rules.py，可从仓库一键重建）。

## 退化 goal 处理（写死）

若某 goal 在 rules64 GT 闭包下只有基图答案或无答案：**不换 goal**，标
degenerate，其 derived-only recall 记无定义（n/a），all-answer recall
照常计算。5 个 goal 全部保留，避免挑选偏差。

## 其余 setting 不变

矩阵、预算、wall、cap、depth=64、种子方案、指标与判据均按 NOTES.md；
summary.md 将明确标注本轮为 rules64（或 rules32），**不与 1,652 规则
的旧结果混比**。
