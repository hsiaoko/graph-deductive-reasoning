# 开放变元 goal 实验设计（预注册）

日期：2026-10-30。动机：Overall 的 50 goal 全部是双常量接地 goal，
backward 的优势结论只覆盖该形态。本实验检验方向选择在
**goal 变元化**后的变化，验证 `docs/direction/README.md` 的
case 1/2/4 预测。

## 1. 研究问题

1. goal 从双常量接地变为单变元/双变元后，backward 的命中与成本
   如何退化？forward chase 是否反超？
2. hub 常量是否显著削弱 backward 的 pin 剪枝优势？
3. "backward 筛规则 + forward 求值"的简易混合是否能在开放 goal 上
   同时优于两个纯方向？（若混合原型实现成本可控）

## 2. Goal 设计（fb15k-237，rules_g3 81,683 规则）

在 Overall 的 50 个接地 goal 的谓词中分层选取，构造三类变体：

| 类型 | 形式 | 数量 | 分层依据 |
|---|---|---|---|
| G1 单变元 | `r(x, B)`（dst 开放） | 10 | 结果集大小（小 <10 / 中 10–1000 / 大 >1000 各至少 2 个） |
| G2 双变元 | `r(x, y)` | 5 | 谓词 fan-in（head 规则数多/少） |
| G3 hub 接地 | `r(H, B)`，H 为高出度 hub 实体 | 5 | 与 G1 同谓词便于对照 |
| G0 接地对照 | `r(A, B)` | 复用 Overall 对应 goal | 基线 |

所有 goal 的 ground truth：capped chase 闭包（2 轮/100k/30M）中
该谓词的全部事实；超出截断的 goal 记 unreachable_by_cap 并单独说明。
hold-out：对接地对照沿用逐任务留出；开放 goal 不删边（求外延而非
证明缺失边），口径在报告中写明。

## 3. 方法臂

1. **backward**（现状直接跑变元 goal；pin 只剩单侧或无 pin，
   budget=10, cap=8）。若当前实现不支持变元 goal，先加最小支持
   （goal 解析 `_` 已有；pin 逻辑按常量有无跳过），默认行为不变。
2. **forward chase**（2 轮截断，CPU 8 线程）：一次运行覆盖全部
   goal，记录总耗时与摊销耗时。
3. **混合（可选，原型）**：backward 的规则可达性分析筛出 goal 相关
   规则子集（DG 反向可达 + 常量约束），在该子集上跑 forward chase。
   标记 hybrid-prototype，不作为正式功能。

## 4. 指标

- **time-to-full-answer**：得到该 goal 全部有效绑定的时间（backward
  需枚举全部解而非首解，若当前实现只支持首解，先记录 time-to-first
  并单独标注，再决定是否扩展 full-enumeration 模式）
- 答案完备率：|backward 答案 ∩ ground truth| / |ground truth|
- 派生事实总数、峰值内存、单 goal 摊销成本
- G3 专项：同一谓词下 hub vs 非 hub 的 backward 成本比

## 5. 预注册判据

- 若 G1/G2 上 forward 摊销成本 < backward 且完备率更高 →
  case 1 预测成立，论文按场景分工写方向选择。
- 若 backward 在 G1 小结果集上仍显著更快 → backward 的适用边界
  比预期宽，修正 README 的 case 1 表述。
- G3 若 backward 成本相对同谓词非 hub 上升 <2× → hub 边界不显著，
  删除 limitation 候选；上升 >10× → 坐实 limitation。
- 混合原型若在 G1 大结果集上同时优于两臂 → 立项为正式方向。

## 6. 执行约束

看门狗（MemAvailable<100GB kill）、重任务串行、单 goal 30min 超时、
固定 seed、零真实 DIVERGENT、结果写入 `experiments/open_goals/`
（summary.md + 明细 + 复现命令）。

## 7. 预期风险

- backward 变元 goal 可能退化为全图枚举 → 超时即数据，不硬跑。
- chase 闭包截断导致大外延谓词 ground truth 不全 → 单独分组，
  不与完备率混算。
- 混合原型若规则子集仍过大，退化为纯 forward，需如实标注。
