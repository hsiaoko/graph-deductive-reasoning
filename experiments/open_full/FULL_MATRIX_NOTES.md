# full_matrix：大答案集完整 baseline 矩阵（预注册）

日期：2026-10-30。setting 由用户拍板；判据先写死后执行。

## Setting

- fb15k-237 全基图 × 1,652 规则子集。Goal = open_full 的 4 个大答案集
  goal：film/language(_,/m/04306rv)（GT 2,052）、position(_,_)
  （9,624）、release_region(_,_)（856,691）、contains(_,_)
  （1,490,045）。GT = 基图 ∪ capped chase（10M/4 轮；30M 高帽闭包可
  用于 beyond-GT 核验）。chase 基线已有：89.3s 完备。
- 全部臂 budget=1e9（实现上的"无限"）+ 单 goal 30min 墙钟超时；
  **超时即数据**（记 timeout 时刻的 recall）。已有 budget=10000 数据
  不作数，全部按新 budget 重跑。

## 臂（6 跑 + 2 N/A）

1. backward + score（--backward-full-enum，collect/match cap 拉满）
2. backward + random（seed 42；此前 3 seed 几乎无差）
3. backward + hybrid
4. backward + HDC scorer（--scorer hdc --hdc-dim 512）
5. forward greedy + deterministic + exact + --forward-full-enum
   （本次新增最小开关；默认逐字节回归）
6. forward greedy + deterministic + HDC scorer（同 5，hdc 512）
7. forward bnb + utility / 8. bnb + hdc-value：**N/A**——单链最优证
   书语义，不回答全外延问题；用文字说明，不跑。

## 判据（写死）

- 每 goal × 臂：recall（chase=1，含基图答案）、wall（fixpoint 时刻或
  30min 超时）、派生数、终止原因（fixpoint/budget/timeout）。
- 汇总：方法为行——平均 recall、平均 wall；另附逐 goal recall 表。
- soundness：派生 ⊆ 闭包（30M 高帽核验 beyond-10M 部分），零真实越
  界；--forward-full-enum 默认行为逐字节回归（synth）。
- 预期（预注册）：HDC 两臂无增益（scorer hdc 更慢、recall 不升）；
  **核心未知：forward greedy 全枚举的覆盖率与速度**——greedy 每步只
  提交 top-1 候选，无 backtracking，预期大 goal 上要么 fixpoint 提前
  （候选池 cap 截断）、要么每答案成本远高于 backward。
