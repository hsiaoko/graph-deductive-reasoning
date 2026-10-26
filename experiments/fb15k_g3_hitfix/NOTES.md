# hitfix 实验进度 NOTES

## 2026-10-26 根因分析（已完成，证据在 logs/）

方法：/tmp/root_cause.cpp（不进仓库）——对每个失败任务，找出 head 谓词 ==
goal 谓词的规则，用 PatternMatcher 全量枚举（cap 500 万），定位"能一步推出
goal 的 embedding"在枚举序中的位置。

**根因（一句话）**：goal motif 从未进入候选列表——生成阶段的
--max-candidates-per-rule 8 把它截掉了，打分器根本没机会看到它。

证据（logs/rootcause_govt.txt, rootcause_profession.txt）：
- 任务 form_of_government(/m/05c74,/m/06cx9)：73 条规则的 head == goal 谓词，
  其中 34 条在基图上有一步派生（X 全过），但 goal embedding 的枚举位置全部在
  288~4306 之间，cap 8 全部截断（34/34 TRUNCATED，0 in-cap）。
- 任务 profession(/m/0161c2,/m/0dz3r)：1529 条 head 规则，61 条可一步派生，
  goal embedding 位置 414~16086，61/61 TRUNCATED。
- 即：cap 8 下候选 = 每规则枚举序前 8 个 embedding（按 slot pair 排序），与
  goal 无关；打分/decoy 分析是次要的——正确 motif 根本不在候选集里。
- 附带确认：这些 goal 边本身在基图中（hold-out 语义下被删），premise 均为
  基图边 → 全是 1 步可派生。

## 修法（进行中）

反向目标制导（用户建议方向，与证据吻合）：
1. graph::PatternMatcher 加 pins 重载（结论槽位常量绑定，默认行为不变）。
2. 新 BackwardExecutor：从 goal 反向——只对 head==goal 谓词的规则做 pinned
   枚举，候选空间塌缩到"能推出 goal 的 motif"；前提缺边且可派生时递归子目标。
