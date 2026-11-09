# hdc_hard_goals：实验 B —— 困难固定实例证明搜索（预注册）

日期：2026-11-01。依据 EXPERIMENT_PLAN.md §实验B。判据写死后不移动。
本实验**不改 git**（用户要求保留工作区变更、不 commit/push）。

## 冻结 goal cohort（先冻结再跑 HDC；不按 HDC 结果淘汰）

复核锚点 4 个（全部保留，含负面对照）：
- `athletes`（g0）=
  /olympics/olympic_participating_country/athletes./olympics/olympic_athlete_affiliation/olympics(/m/05r4w,/m/09n48)
  ——历史低收益对照（utility 2084 展开，hdc 2058，-1.2%）
- `r2c_2` = /location/hud_county_place/county(/m/0r540,/m/0l2rj)
  ——历史 -93%
- `r2c_4` = /music/genre/artists(/m/0fd3y,/m/01vvycq) ——历史 -93%
- `r2c_8` = /film/film/runtime./film/film_cut/film_release_region(/m/04v8x9,/m/02w7gg)
  ——历史 -31%
（goal spec 逐条从 .work_hdcv/{r2c_2,r2c_4,r2c_8,g0}.log 的 goal 行核对，
非凭记忆。）

新增 goal：本轮**不新增**（补选需按预注册的可观测困难度特征 + 训练隔
离，时间盒内不可靠补齐则明确 pending，不伪造）。

## 设置（固定，与 hdc_online 主 A/B 完全一致）

fb15k-237 全图 × 原规则集 1,652 条（DG arcs=3023）；budget=2、cap=64、
hybrid/exact、top_k=0、串行计时、单 run 外部 timeout 2000s、看门狗
MemAvailable<100GB、全机红线 400GB。

## 臂（同图/规则/goal/budget/cap/policy/scorer，只换分支优先级）

1. **BNB-utility**：priority = cost + minprice（现状默认）。
2. **BNB-HDC-value**：`--bnb-priority hdc-value --value-model
   .work_hdcv/models/<goal>.meta`（hdc_online 的 LOGO 模型，原样复用：
   训练构成 trained_on 见各 meta；测试 goal 不在其训练集）。
3. **BNB-numeric-value**：`--bnb-priority numeric-value --value-model
   <numeric LOGO 模型>`——同一批 [bnbl] trace、同一 LOGO 隔离、同一
   11 个原始特征 + 同一 4 个交互项；数值 ridge（lam=1.0 同 HDC 版），
   标准化均值/方差只在训练集拟合（写入 meta）。交互项 = 标准化后的
   特征两两乘积（CONJ 同序）。
4. **backward-deterministic** 参照：`--direction backward --executor
   backward`（完整 DFS+回溯、score 序），budget=2。成本口径（写死）：
   **proof_cost = chain.tsv 末行 cum_cost**（证明本身的成本，跨执行器
   可比）；summary `total_cost` = search_spent（含失败应用的搜索计
   费），**不是** best/proof cost，不得混用；certified 记 N/A。CLI
   budget 相同 ≠ 记账语义相同。
5. **chase**：N/A——1,652 规则 uncapped fixpoint 已在
   full_policy_matrix 证实资源包络内不可行（round-4 +59.0M、threads=8
   round-5 RSS 312GB 逼近红线；不重复危险实验，不借用 rules512 的
   13.58s 冒充）。BNB expansions 对 chase/backward 填 N/A 不填 0。

## 判据（写死，沿用原预注册）

- HDC 主 A/B 成功 ⟺ 相对 utility：展开数降 ≥20%、端到端 wall 不劣化、
  best_cost/certified/推导链一致。
- numeric-value 单独报告：检验收益是否专属于 HDC 表示（不预设 HDC 必优）。
- 锚点复核与新 goal 泛化分开报告（本轮无新 goal → 仅锚点复核）。
- 正确性：每 run 派生 soundness（⊆ 该规则集可得闭包或等价核验）；
  默认路径回归逐字节（synth bnb）。
- numeric 模型正确性：C++ 预测与 Python 参考实现逐值一致（<1e-9）；
  加载校验（格式/键序/标准化维度不符即拒绝）。

## 训练构成（冻结）

LOGO per goal：成功集 {g0,g6,g9,g22,r2c_2,r2c_4,r2c_8} − {该 goal} +
全部失败/熔断 trace（.work_hdcv 的 g*.log/r2c_*.log 其余）。numeric 与
HDC 共用同一份训练行（同 trace、同标签口径：最优路径上 y=best−cost，
其余 3.0；失败 trace 全 3.0）。标准化参数逐 goal 模型各自在其训练行
上拟合。

## 产出

`experiments/hdc_hard_goals/`：本 NOTES、train_numeric.py、
verify_numeric.py、逐 goal 表 summary.md、复现脚本。工作产物
`.work_hdcv/hdc_hard_goals/`。
