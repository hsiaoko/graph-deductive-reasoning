# 推理方向选择：backward vs forward 的场景分析

日期：2026-10-30。本文沉淀 Overall 实验与 Phase 1–3 之后的方向讨论结论。
相关：`experiments/fb15k_overall/`、`experiments/backward_hdc/`、
`experiments/instance_hdc/`、`experiments/hard_ordering/`。

## 1. 技术分层（贡献主次的实证结论）

```
第一性：搜索空间/方向（backward pinning、AND-OR 分解）   ← 数量级收益
第二性：搜索纪律（DFS+回溯、分支定界、下界剪枝）          ← 结构性收益
第三性：排序器（scorer、HDC 价值模型、学习排序）          ← 残余空间的边际优化
```

推论：给次优配置做加速不构成系统贡献。HDC 价值函数在前向 bnb 上
−87% 展开是真实优化，但 backward-bnb 在 athletes 上 2084→2 展开，
使其优化对象失去必要性。forward greedy 的 0/50 是候选池截断问题，
不是打分问题；forward bnb 准确但昂贵。

## 2. 六个场景 case（谁该用什么）

| # | 场景 | 选择 | 理由 |
|---|---|---|---|
| 1 | 开放 goal：`award(x,B)` / `award(x,y)` | 小答案集 backward / 中大答案集 forward | 实测（open_goals）：答案集 <10 时 backward 仍更快（0.1–1.9s 内全解）；中/大答案集（43–2000+）backward 只给前缀（完备率 0–12%），forward 一次闭包是全外延唯一可行路径 |
| 2 | 批量 goal / 全闭包物化 | forward chase | 摊销优势：199.5s/50 goal = 4.5s/goal，goal 越多越划算；下游 ML/挖掘 support 需要全量派生 |
| 3 | 探索性推理（异常检测、知识发现、规则挖掘） | forward | 没有 goal 可反向 |
| 4 | hub 常量 goal（超级枢纽实体） | **backward（grounded 时反为优势）** | 实测（open_goals G3）：hub（deg 4482–5476）spent=1/~40ms 一击即中，非 hub 对照贵 27–620× 且可 MISS——hub 目标边多在基图或一步可达。hub 不是 grounded goal 的 limitation；潜在风险在 hub 作开放中间变量（未覆盖） |
| 5 | 深层递归闭包（传递闭包、可达性） | forward / 混合 | 半朴素求值每事实只推一次全体复用；backward 递归树爆炸（film profile：递归子目标搜索 99.5%） |
| 6 | 单个/少量接地 goal | **backward（主线）** | goal pinning 使候选空间塌缩；20/50 规模 92% 命中、中位 0.25s |

最优链证书场景：backward-bnb（简单形态已验证；协调形态已入空间，
稠密递归下 film 级 goal 仍超时）。

## 3. 混合路线（Magic Sets 思想，后续方向）

数据库 top-down/bottom-up 之争的标准综合是 Magic Sets：
**用 backward 的目标约束改写规则集，再用 forward 执行**。
本仓库的自然落地：

```
backward 分析 goal → 筛出 goal 相关规则子集 + 可 pin 的常量约束
→ 在子程序上跑 forward chase（可 GPU）
→ 开放 goal / 批量 goal 也享受目标制导剪枝
```

这条路线同时覆盖 case 1/2/5，是把两个方向从"对立"变成"组合"的
关键工程候选。

**实测注记（open_goals）**：仅做"DG 反向可达规则过滤"的简易混合原型
**退化**——深度 1 的反向可达子集仍含 88.4% 规则（chase 218s vs 纯
forward 215s，无收益）。Magic-Sets 式的**常量约束改写**（而非单纯
规则过滤）需要另行立项，不能指望 DG 剪枝单独奏效。

## 4. 实例级瓶颈的实证记录（防止重复踩坑）

- film 超时 ≠ 规则扇出（0.4%）≠ 实例 join 枚举（≈0%）；
  = 递归子目标搜索树（99.5%）。攻法：递归剪枝强度 × 预检成本
  （实例下界已在 award 上证明：830 剪枝换 21s 可解）。
- 学习排序在 backward 的 OR 尝试上两轮皆败（正例稀缺 + 启发式已强）。
- HDC 唯一正向角色 = 前向 bnb 价值函数（hdc_online），系统价值受
  方向替代影响，定位为对照/附录。
