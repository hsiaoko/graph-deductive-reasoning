# hard_ordering：硬场景下学习排序是否重新有用（预注册）

日期：2026-10-29。紧接 4c624a5（协调形态入证书空间；film 因 666 条 head
规则穷举超时）。

## 判据（写死）

学习排序在硬场景有效 ⟺ 同时满足：
1. 命中/可行性改善（此前超时/未命中 goal 救活 ≥1，原命中零丢失）；
2. 同命中集合 wall/展开数不劣化（平均改善 ≥20% 为佳）；
3. 正确性不变（best_cost 一致、certified 口径不变、派生 ⊆ 闭包、默认
   路径逐字节）。
只改善耗时但无命中改善且收益 <20% = "边际价值"，如实写。

## Workload

- A 层（主引擎 backward 首解）：Overall 未命中 4 goal（tasks50 索引
  41/43/48/49）+ film/award 协调 goal + goal20（232 展开）+ 大扇出 goal。
- B 层（backward-bnb 证书）：film、award、athletes、goal20。

## 臂

- heuristic（现状）vs hdc-value（--backward-priority hook，LOGO 模型）。
- B 层追加工程对照臂：惰性枚举（按规则级下界 est_rule 升序、按需枚举、
  est_rule >  incumbent 即停——证书精确，因为 est_rule 是该规则任何候选
  成本的下界）——区分"学习排序收益"与"少枚举收益"。
