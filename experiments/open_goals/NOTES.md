# open_goals：开放变元 goal 实验过程记录（2026-10-30）

## 资产

- ground truth：base_edges.tsv（289,650 基图边）∪ g3 全量 chase（2 轮，
  100k 匹配，30M 派生上限，STOPPED by cap；23,483,693 派生事实，~230s
  一次）——合并为 ground truth。chase 输出只含派生（round≥1），基图边
  另行并入（过程记录：首版分层只用了派生，后修正为基图+派生；被分类的
  (pred,dst) 对恰好全部无基图边，分层数值未变）。
- goal 文件：.work_hdcv/open_goals.tsv（G1×10 按答案集分层、g2_run.tsv、
  g3_pairs.tsv）；.work_hdcv/goals50.tsv（G0 复用 Overall）。
- 最小扩展：--open-collect-cap（默认 8 逐字节不变，commit 已入）；
  backward 原生支持 `_` 变元（synth 冒烟过）。

## 中间发现

- backward 在开放 goal 上受 budget 约束成为"部分答案器"：collect cap 不是
  瓶颈（cap=100 与 cap=8 答案数相同），budget=10 才是。
- G3 hub/ctrl（逐任务留出，同谓词同 dst）：hub 全 spent=1（~40ms 一击），
  ctrl spent=2-10、27-620× wall，1 个 MISS——hub 边界假设被反转（grounded
  hub goal 更便宜：hub 的目标边往往基图或一步可达，非 hub 需深链）。
- 混合原型：DG 反向可达子集（15 goal 谓词）深度 1 含 72,237/81,683 规则
  （88.4%），深度 2 覆盖 100%——DG 过稠，按文档预警"退化为纯 forward"。
