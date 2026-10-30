# open_full：全外延对比实验（backward 全枚举 vs forward 全量 chase）

日期：2026-10-30。预注册判据：NOTES.md。最小扩展 commit a49df84
（--backward-order / --backward-full-enum，默认逐字节不变）。

**Setting**：fb15k-237 全基图；规则 1,652 子集；12 个开放 goal
（G1 r(x,B)×9 + G2 r(x,y)×3，子集 GT 答案集重分层：小 4 / 中 4 /
大 4；3 个空答案 goal 剔除）。GT = 基图 ∪ capped chase（10M 派生
截断、4 轮；trunc_risk=possible 的 goal 其 GT 为下界，beyond_gt 列
单独计数）。臂：backward 全枚举（budget=10000，collect-cap/match-cap
拉满，30min 超时）× {score / random×3seed / hybrid}；forward = 一次
全量 chase。

**计时基线（forward chase）**：**89.3s 端到端**（计算 ~11s + 闭包写
盘 ~75s），峰值 RSS 8.8GB，产出 10M 派生事实 = 全部 12 个 goal 的
完整答案集（截断风险见各 goal 标注）。

## 分层对照表（每 goal × 臂：recall / wall / spent）

（跑完后从 .work_hdcv/of_detail.tsv 填入；当前为中间发现）

## 中间发现（待全量确认）

- 小层：award_nominations（GT=5）1 次应用 100%——backward 完胜形态；
  award_winning_work（GT=1）全臂 0%（fixpoint 推不出 round-4 深度答
  案）——backward 在小层也会失败；awards_won（GT=6）100% 但 spent
  ~7.6k（大量失败应用）。
- 中层：profession（GT=51）100%（~950 spent，20s，另有 904 条
  beyond-GT 属 GT 截断）；places_lived（GT=243）score/hybrid 0%、
  random 2/3 seed 51.4%——**score 序主动卡死，随机反而逃出**。
- 大层：film/language（GT=2052）全臂 ~24% fixpoint；contains（GT
  149 万）全臂 1.04%（budget 顶满，~220s）。
- 结论方向：小/中层 backward 可比 89s chase 快得多且完备；大层
  forward 一次闭包是全外延唯一可行路径（与 open_goals case 1 一致）；
  policy 在 backward 全枚举下整体不敏感，但在特定 goal
  （places_lived）上 score 序会卡进失败子目标循环而随机序逃出。
