# hdc_value2：HDC 跨 goal 预测优势的小规模严格验证

日期：2026-10-28。前轮（experiments/hdc_value/）：不可行，但 LOGO 上 HDC
0.491 vs 工程 0.279 是唯一正向信号，样本只有 1 个非平凡 goal。本轮补强
非平凡 goal 样本 + one-hot 通用基线。

## 预注册判据（分析前写死）

HDC 可行 ⟺ 同时满足：
1. LOGO 跨 goal 预测（Spearman）显著优于工程特征 ridge 与 one-hot ridge
   两者；
2. 离线重放到达最优解的展开数相对最佳基线再降 ≥20%。
只赢预测不赢重放 = 不可行；赢 one-hot 但输工程特征 = 记录为"通用表示内
最优但无系统价值"。

## 数据采集方案

- gen-goals 在 rules（3023 条）上 max-rounds 2 → 取 round=2（需 2 步派生）
  的 goal；bnb budget=2、cap=64、GAR_DEBUG_BNB 轨迹。
- 非平凡性证据：goal 父分支的首次展开位次 > 1（trace 直接读）。
- 每个 goal 运行小而快；不可行就换，不硬啃。budget-3 树爆炸，不碰。
- 安全：看门狗 MemAvailable<100GB kill gar_reason；低并发；不碰
  rules_g3 大 budget 与 chase 2M。

## 结果记录（2026-10-28）

- gen-goals 的 round-2 goal 全部已在基图（goal_already_in_graph）——不可作
  目标；改用 .baseline/fb_deduce_cpu（rules 集 2 轮闭包）的 round=2 行
  （与自跑 1 轮闭包的 round-1 计数精确一致：205,701）。
- 成功 7 个（4 非平凡：direct 2084/18962/12727/46），失败+在跑 40 个。
- 预测：hdc 0.378 > onehot 0.305 > eng 0.260；hdc vs eng Wilcoxon
  p=0.016，hdc vs onehot p=0.34。重放：hdc 比最佳基线（eng）再降 82.5%。
- 关键分支首选命中率全模型 ≈0（1/9 上限）——收益来自全局排序。
- 种子稳健性：两 hash 种子输出逐位一致（大 n 下随机特征 ridge ≈ 确定性
  核机）。
