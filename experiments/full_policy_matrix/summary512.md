# full_policy_matrix @ rules512（最终：110/110 完成）

日期：2026-10-31。预注册：NOTES.md / RULE_SUBSET.md / RULE_SCALE512.md
（判据未移动）。**状态：110/110 matrix runs + 全部计时重复（共 180 个
summary）完成。3 个 timeout run（recall@1800s，见下）——超时即数据，
未跳过。**

## Setting 执行核对

- fb15k 全图 × rules512（前缀嵌套程序验证：manifest512[:256] ==
  manifest256 逐条一致；64⊂128⊂256⊂384⊂512）。**未降级到 384。**
- 参数不变：budget=1e9、wall=1800s、cap=100000、open-collect-cap 1e7、
  depth=64（无 run 触及）、HDC512、hdc-seed=42、random seeds {7,13,42}、
  det/hyb 计时 3 次 median。
- soundness：**全部 110 run（含 3 个 timeout run）派生 ⊆ GT512
  fixpoint 闭包，零越界**；precision 全 1.000；无 run 报 cap（
  fwd_capped_enums 全 0 于 fixpoint 终止处记录）与 depth 截断。
- timeout（1800s 墙钟，recall@1800s）：**g5 fwd_hdc_rand_s7 =
  0.663/0.580、s13 = 0.679/0.600、s42 = 0.686/0.608**（all/drecall，
  derived 34,374–36,281）。其余 107 run 全部 fixpoint 终止。

## GT @ rules512（无 cap 真实 fixpoint）

| | 值 |
|---|---|
| 轮数 / 派生 | **22 轮 / 349,552，fixpoint=true，零 cap** |
| chase 时间（两口径） | **完整闭包 T = 13.58s（threads=1）**；**batch 摊销 T/5 = 2.716s/goal**；threads=8 参考 3.32s（闭包逐字节一致） |
| 峰值 RSS | 237MB |
| GT 答案 | g1=5(4+1)、g2=6(0+6)、g3=955(8+947)、g4=0（degenerate，排除宏平均）、g5=11,464(2,273+9,191) |

## 逐 goal 表（recall / derived-only recall；各臂一致处合并）

| goal | backward（7 臂逐数一致） | forward |
|---|---|---|
| g1 (GT=5) | 1.000/1.000 @0.001s | 全 15 配置 1.000/1.000 @0.001s |
| g2 (GT=6) | 1.000/1.000 @4.8s | 1.000/1.000；det null 10.1s、exact 38.5s、hdc 230.3s；rand 更慢（见宏表） |
| g3 (GT=955) | 1.000/1.000 @0.104s | 1.000/1.000；det null 0.050s、exact 0.113s、hdc 0.631s |
| g4 (GT=0) | n/a，prec 1.0，0 派生 | n/a，prec 1.0，毫秒级 |
| g5 (GT=11,464) | **0.470 / 0.338 @0.57s**（7 臂逐数一致，der=3,109） | det/hyb/random（null/exact）1.000/1.000；**hdc random ×3 timeout@1800s：0.663–0.686 / 0.580–0.608** |

## 宏平均表（成员 {g1,g2,g3,g5}，g4 空排除；det/hyb 先 3 次 median、
random 先 3 seeds median，再对 goal 不加权平均；random 的 timeout 以
recall@1800s 计入——预注册口径）

| 方法（X） | all recall | drecall | precision | wall mean (s) | derived mean | 派生缩减率† |
|---|---|---|---|---|---|---|
| **chase（GT 基线）** | 1.000 | 1.000 | 1.000 | **T=13.58s（完整闭包）/ 摊销 2.716s** | 349,552 | 0.000（定义） |
| bwd score | 0.867 | 0.835 | 1.000 | 1.376 | 1,016 | **0.997** |
| bwd random×3 | 0.867 | 0.835 | 1.000 | 1.397 | 1,016 | 0.997 |
| bwd hybrid×3 | 0.867 | 0.835 | 1.000 | 1.373 | 1,016 | 0.997 |
| fwd null det | 1.000 | 1.000 | 1.000 | 5.051 | 24,333 | 0.930 |
| fwd null hybrid | 1.000 | 1.000 | 1.000 | 6.873 | 24,333 | 0.930 |
| fwd null random×3 | 1.000 | 1.000 | 1.000 | 89.783 | 24,332 | 0.930 |
| fwd exact det | 1.000 | 1.000 | 1.000 | 21.082 | 24,332 | 0.930 |
| fwd exact hybrid | 1.000 | 1.000 | 1.000 | 15.706 | 24,332 | 0.930 |
| fwd exact random×3 | 1.000 | 1.000 | 1.000 | 181.946 | 24,332 | 0.930 |
| fwd hdc det | 1.000 | 1.000 | 1.000 | 99.766 | 24,332 | 0.930 |
| fwd hdc hybrid | 1.000 | 1.000 | 1.000 | 98.722 | 24,332 | 0.930 |
| fwd hdc random×3 | **0.920** | **0.900** | 1.000 | 528.803 | 18,132 | 0.948 |

† 派生缩减率 = 1 − D_agent,g / D_chase_total（349,552）；逐 goal 先算
再 macro mean（未先合并再除）；chase 基线 0 定义。

## 关键结论（512 档，含与 256 对照）

1. **方向边界随 GT 规模反转被定量坐实**：backward 在 {g1,g2,g3} 保持
   1.000 且派生缩减率 0.997（只推答案所需，macro wall 1.4s）；但 g5
   覆盖率随 GT 增大而坍塌——256 档 0.974/0.955（GT 5,527）→ 512 档
   **0.470/0.338**（GT 11,464，7 臂逐数一致、非 depth 截断）。**大答
   案集上 backward 结构性失效，forward 增量引擎在全部规模下完备。**
2. **policy 只分化搜索成本不分化 recall**（唯一例外见 3）：forward
   random 的 g5 wall 比 det 慢 24×（exact：1114.9s vs 46.6s）至 34×
   （null：339.0s vs 10.1s），最终 fixpoint 与 recall 逐数一致。
   backward 各臂时间/答案无可见分化。
3. **HDC 纯开销的边界形态**：hdc det/hyb recall 与 exact 逐数一致、
   macro wall ~4.7×（99.8s vs 21.1s）；**hdc + random 是唯一跌破
   1.000 的组合——g5 三 seed 全部 1800s timeout（recall@1800 =
   0.66–0.69）**，慢打分 × 随机绕路 = 时间预算内不可完备。
4. chase 两口径：完整闭包 T=13.58s（recall=1 定义）；摊销 2.716s/goal
   ——若只取一个 chase 值用于"单 goal 全外延"，T 是正确口径（agent
   的最快完备臂是 fwd null det macro wall 5.05s，仍快于 T，但慢于摊
   销口径）。
5. soundness：全部 110 run（含 timeout）派生 ⊆ GT fixpoint，零越界；
   precision 全 1.000。

## 复现 / 续跑

全部完成，无 pending。复现链（与 256 同构）：
`select_rules.py <root> <out> 512` → GT gate（rules512/gt_closure512）
→ `run_matrix512.sh`（串行断点续跑）→ `aggregate512.py`（最终
MACRO_GOALS=[g1,g2,g3,g5]）。产物：
`.work_hdcv/full_policy_matrix/{rules512,gt_closure512,runs512,
manifest512.tsv,gt_answers512.tsv,detail512.tsv,agg512_final.txt}`。
