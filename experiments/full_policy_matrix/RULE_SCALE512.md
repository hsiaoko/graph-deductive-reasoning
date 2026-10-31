# 预注册补充 3：规则规模扩展 256 → 512（用户授权"取平均值，用多一点规则"）

日期：2026-10-31。NOTES.md / RULE_SUBSET.md / RULE_SCALEUP.md 保留有效；
本文档写死 512 档算法、阶梯与**宏平均口径**，先于任何生成与运行。
禁止按各推理臂表现挑规则或移动球门。

## 阶梯（仅一次降级）

1. **首选 N=512**（rules512）。GT 门槛不变：fb15k 全图、无有效 cap 的
   真实 fixpoint（max-rounds 1000、max-derivations/matches 1e9、
   print 0、threads=1 主计时、threads=8 附加参考），1800s + 400GB
   红线 + 看门狗，串行。
2. 若 512 不可行 → **同一有序序列的前 384 条**（rules384，仍 >256）。
3. 若 384 也不可行 → 停止并如实报告 blocked。

嵌套保证（写死）：**rules256 ⊂ rules384 ⊂ rules512 前缀嵌套**——选择
算法不变（top-4 direct head + DG 祖先池 round-robin，排序键
confidence desc / support desc / rule_id asc），循环上界改 512，前 256
条逐字节不变。生成后程序校验 manifest512 前 256 行与 manifest256 的
rule_id 序列逐一相同（且 64 ⊂ 128 ⊂ 256 链条保持），不一致即停止。

## 矩阵参数（不变）

budget=1e9、wall=1800s、cap=100000、open-collect-cap 1e7、depth=64、
HDC dim=512、hdc-seed=42、forward {null,exact,hdc}×{det,random,hybrid}
（random seeds {7,13,42}）+ backward exact×{score,random,hybrid}
（random/hybrid 各 3 seeds）= 22 runs/goal；det/hyb 计时 3 次取 median
（单 run>10min 可一次并说明）；串行断点续跑，goal 1→5。

## 宏平均口径（用户新要求，写死）

- **成员资格（由 GT 决定）**：只纳入 GT 中 derived answer count > 0 的
  非退化 goal。512 GT 产出后按其答案统计写死名单；若 g4 在 512 仍空
  则排除（预期 g1/g2/g3/g5 纳入）。名单确定后不因结果调整。
- **顺序**：每个 goal 内先聚合（det/hyb 3 次计时取 median；random 3
  seeds 取 median），再对 goal 做**不加权 macro mean**。指标：
  all-answer recall、derived-only recall、precision、wall mean、最终
  derived 数 mean、逐 goal 派生缩减率
  (1 − D_agent,g / D_chase_total) 的 macro mean。**禁止先合并答案/
  派生再算比例**（避免大 goal 支配）。
- chase 基线：同一 full closure 是 GT——recall=1（定义）、派生缩减率
  基线 0。chase 时间同时报两个口径：**single-goal 完整闭包成本 T** 与
  **batch 摊销 T/5**；任何单一 chase 数值必须标注口径，不混淆。
- 汇总形态：X=baseline（方法），Y=time / 最终派生缩减率 / accuracy
  三联。random 在各 goal 先 seed median 再入平均。HDC scorer 负结果
  如实；**BNB-HDC 是另一任务，不混入本宏平均**。
- 跨 N 变化只作规模对照（GT 变了），不当严格加速比；不因结果删 goal
  或改平均口径。

## 时间盒

~2h 硬上限：manifest → GT → goal1/2 全矩阵 → 顺序。未跑完明确
pending + 续跑命令，不宣称完整。
