# full_policy_matrix @ rules512（部分完成：95/110；g5 15 runs pending）

日期：2026-10-31。预注册：NOTES.md / RULE_SUBSET.md / RULE_SCALE512.md
（判据未移动）。**状态：g1–g4 完整（88/88），g5 完成 7/22，15 runs
因时间盒 pending（断点可续，见文末）。未宣称完整。**

## Setting 执行核对

- fb15k 全图 × rules512（确定性续选，前缀嵌套程序验证：
  manifest512 前 256 行 == manifest256 逐条一致；64⊂128⊂256⊂384⊂512）。
  **未降级到 384。**
- 矩阵参数不变（budget=1e9、wall=1800s、cap=100000、collect-cap 1e7、
  depth=64 未触及、HDC512、hdc-seed=42、random seeds {7,13,42}、
  det/hyb 3 次计时 median）。
- 完成 **95/110** matrix runs（+ reps 共 157 个 summary）；全部串行；
  无 timeout run（最长完成 run：g5 fwd_exact_rand_s7 = 1114.9s）。
- soundness：95 个完成 run 全部派生 ⊆ GT512 闭包，**零越界**；
  precision 全 1.000。

## GT @ rules512（无 cap 真实 fixpoint）

| | rules512 | rules256（对照） |
|---|---|---|
| 轮数 / 派生 | **22 轮 / 349,552，fixpoint=true，零 cap** | 18 轮 / 222,146 |
| wall threads=1（主） | **13.58s**（single-goal 完整闭包成本口径） | 3.58s |
| wall threads=8（参考） | 3.32s（闭包逐字节一致） | 1.59s |
| batch 摊销口径 | 13.58/5 = **2.72s/goal** | 0.72s/goal |
| 峰值 RSS | 237MB | 207MB |

GT 答案：g1=5(4+1)、g2=6(0+6)、g3=955(8+947) 不变；g4=0 仍 degenerate
（排除出宏平均）；**g5=11,464（2,273+9,191；256 档为 5,527）**。

## 逐 goal 简表（recall / drecall；各臂一致处合并）

| goal | backward 全 7 臂 | forward 全 15 配置 |
|---|---|---|
| g1 (GT=5) | 1.000/1.000 @0.001s | 1.000/1.000 @0.001s |
| g2 (GT=6) | 1.000/1.000 @4.8s | 1.000/1.000；null 10.1s、exact 38.5s、hdc 230.9s |
| g3 (GT=955) | 1.000/1.000 @0.10s | 1.000/1.000；null 0.05s、exact 0.11s、hdc 0.63s |
| g4 (GT=0) | n/a，prec 1.0，0 派生 | n/a，prec 1.0，毫秒级 |
| g5 (GT=11,464，部分) | **pending（0/7）** | 完成 7/15：det 系 recall/drecall 全 1.000；null det 10.1s、null hyb 17.2s、null rand 225.9–367.0s、exact det 46.6s、exact rand_s7 1114.9s |

## 宏平均表（X=baseline；共同完成集 {g1,g2,g3}，g4 空排除，g5 pending
——所有方法同口径 3 goal 不加权平均；det/hyb 先 3 次 median、random
先 3 seeds median）

| 方法 | recall | drecall | precision | wall mean (s) | derived mean | 派生缩减率† |
|---|---|---|---|---|---|---|
| **chase（GT 基线）** | 1.000 | 1.000 | 1.000 | **T=13.58s（完整闭包）/ 摊销 2.72s** | 349,552 | 0.000（定义） |
| bwd score | 1.000 | 1.000 | 1.000 | 1.644 | 318 | **0.999** |
| bwd random×3 | 1.000 | 1.000 | 1.000 | 1.661 | 318 | 0.999 |
| bwd hybrid×3 | 1.000 | 1.000 | 1.000 | 1.641 | 318 | 0.999 |
| fwd null det | 1.000 | 1.000 | 1.000 | 3.374 | 12,372 | 0.965 |
| fwd null hybrid | 1.000 | 1.000 | 1.000 | 3.430 | 12,372 | 0.965 |
| fwd null random×3 | 1.000 | 1.000 | 1.000 | 6.701 | 12,371 | 0.965 |
| fwd exact det | 1.000 | 1.000 | 1.000 | 12.884 | 12,372 | 0.965 |
| fwd exact hybrid | 1.000 | 1.000 | 1.000 | 12.764 | 12,372 | 0.965 |
| fwd exact random×3 | 1.000 | 1.000 | 1.000 | 17.399 | 12,371 | 0.965 |
| fwd hdc det | 1.000 | 1.000 | 1.000 | 76.975 | 12,372 | 0.965 |
| fwd hdc hybrid | 1.000 | 1.000 | 1.000 | 75.570 | 12,372 | 0.965 |
| fwd hdc random×3 | 1.000 | 1.000 | 1.000 | 104.977 | 12,371 | 0.965 |

† 派生缩减率 = 1 − D_agent,g / D_chase_total（D_chase_total=349,552，
chase 基线 0 定义）；逐 goal 先算再 macro mean（未先合并再除）。

g5 部分结果（不计入上表）：完成 7 配置 recall/drecall 全 1.000，wall
null det 10.1s / null hyb 17.2s / null rand median 339.0s / exact det
46.6s / exact rand_s7 1114.9s——**random 的 g5 时间爆炸（null rand
339s vs det 10.1s；exact rand 1115s vs det 46.6s），recall 仍不变**。

## 相对 rules256 的变化（规模对照，非加速比）

1. GT：18 轮/222,146 → **22 轮/349,552**；g5 GT 5,527→11,464（+107%）；
   g1/g2/g3 不变（g3 的 955 在 256 已收敛）。
2. recall 口径无变化：完成 run 全非退化 goal 100%（forward 与
   backward 在 {g1,g2,g3} 上仍全 1.000）。
3. **policy 时间分化急剧放大**（recall 仍零分化）：g2 exact random
   52.1s vs det 38.5s（1.35×）；**g5 规模下 exact random 1114.9s vs det
   46.6s（24×）、null random 339s vs det 10.1s（34×）**——随机排序
   在更大候选空间里显著绕路，但 fixpoint 不变。HDC 仍纯开销
   （{g1-g3} macro：hdc det 77.0s vs exact det 12.9s = **6.0×**，g2 单
   goal 230.9s vs 38.5s）。
4. 方向边界：backward 在 {g1,g2,g3} 保持 100% 且派生极少（缩减率
   0.999——目标制导只推答案所需）；forward fixpoint 推导整个 DG 锥
   （缩减率 0.965）。g5 backward 是否仍 0.97x 待 pending runs 验证。

## Pending（15/110，断点续跑）

g5 未完成配置：`bwd_score`、`bwd_rand_s{7,13,42}`、
`bwd_hyb_s{7,13,42}`、`fwd_exact_hyb`（rep2/3）、
`fwd_exact_rand_s42`、`fwd_hdc_det`、`fwd_hdc_hyb`、
`fwd_hdc_rand_s{7,13,42}`（hdc rand 预计各 10–30min）。

```bash
bash experiments/full_policy_matrix/run_matrix512.sh   # 断点续跑（已完成自动跳过）
python3 experiments/full_policy_matrix/aggregate512.py # 全完成后宏平均自动纳入 g5
#   （届时把 aggregate512.py 的 MACRO_GOALS 恢复为 ['g1','g2','g3','g5']）
```

## 复现

`select_rules.py <root> <out> 512` → GT gate（同 256 命令换 rules512/
gt_closure512）→ `run_matrix512.sh` → `aggregate512.py`。产物：
`.work_hdcv/full_policy_matrix/{rules512,gt_closure512,runs512,
manifest512.tsv,gt_answers512.tsv,detail512.tsv,agg512_final.txt}`。
