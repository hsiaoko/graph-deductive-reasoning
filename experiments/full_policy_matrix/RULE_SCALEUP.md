# 预注册补充 2：规则规模扩展 64 → 256（用户授权"多搞点规则"）

日期：2026-10-31。RULE_SUBSET.md 与 NOTES.md 保留有效；本文档写死扩展
算法与阶梯，先于任何规则生成与 GT 运行。**禁止按各推理臂表现挑规则。**

## 阶梯（仅一次降级）

1. **首选 N=256**（rules256）。GT 门槛不变：无有效 cap 的真实 fixpoint
   （max-rounds 1000、max-derivations/matches 1e9、print 0、threads=1
   主计时、threads=8 附加参考），1800s + 400GB 红线 + 看门狗，串行。
2. 若 256 不可行 → **同一有序序列的前 128 条**（rules128）。
3. 若 128 也不可行 → 停止并如实报告 blocked。

嵌套保证（写死）：**rules64 ⊂ rules128 ⊂ rules256 且为前缀嵌套**——
RULE_SUBSET.md 的算法（top-4 direct head + DG 祖先池 round-robin，排序
键 confidence desc / support desc / rule_id asc）是确定性的，循环上界
从 64 改为 256 时，前 64 条逐字节不变（head picks 相同、round-robin
从第 64 位继续）。生成后程序校验 rules64 == manifest256 前 64 行
（rule_id 序列逐一比对），不一致即停止报告。

## 规模对照口径（写死）

- 图、5 goals、22 runs/goal、budget=1e9、wall=1800s、cap=100000、
  open-collect-cap=1e7、depth=64、HDC512、种子方案全部不变。
- 跨 N 比较只看：GT 规模/轮数、degenerate 变化、recall 变化、wall 变化
  趋势、policy 是否开始分化、HDC 是否仍纯开销、forward/backward 方向
  边界。**不把不同 N 的绝对时间当严格加速比**（答案集变了）。
- degenerate goal 按原判据保留（不换 goal）；若 g3/g4 在 256 下变为非
  degenerate，如实记录。
- 报告区分最终 recall 与搜索成本；policy 无 recall 分化不等于无时间差；
  chase 只报总时间与 /5 摊销。

## 时间盒与优先级

~2h 硬上限：manifest → GT → goal1/2 全矩阵 → 顺序 goal3/4/5。未跑完明
确 pending + 续跑命令，不宣称完整。
