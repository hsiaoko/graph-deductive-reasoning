# full_policy_matrix：rules64 完整矩阵（chase/backward/增量 forward × policy × scorer）

> **规模说明（2026-10-31 更新）**：本文件是 **N=64 中间档**。规模序列
> 64 → 256（`summary256.md`）→ **512（`summary512.md`，最新）**。
> 请勿把本文件当作最终规模引用。

日期：2026-10-31。预注册：NOTES.md + RULE_SUBSET.md（判据未移动）。
**本轮为 rules64（64 条确定性子集，manifest.tsv 可审阅/可重建），不与
1,652 规则的旧结果混比。** 子集选择与 GT 门槛见 RULE_SUBSET.md。

## Setting 执行核对

- 图 fb15k-237 全图（289,650 边）× rules64；no holdout；5 goal 全保留。
- Agent：budget=1e9、wall-limit=1800s、cap=100000、backward
  open-collect-cap=1e7、depth=64（无任何 run 触及，详见各 run
  summary 的 max_subgoal_depth=64；无 depth_limited 事件）、HDC dim=512、
  hdc-seed=42；det/hyb policy-seed=42，random {7,13,42}。
- **110/110 matrix runs 完成**（+70 个 det/hyb 计时重复，共 180 个
  summary）；全部串行；无 timeout run（最大单 run 20.7s——计时重复
  全部 3 次取 median）。
- soundness：**全部 110 run 的派生 ⊆ GT fixpoint 闭包，零越界**；
  全臂全 goal answer-precision = 1.000。

## GT（rules64，无 cap 真实 fixpoint）

| | 值 |
|---|---|
| 轮数 / 派生 | **8 轮 / 25,515 条，fixpoint=true，零 cap** |
| wall（threads=1 主计时） | **0.46s**（threads=8 参考 0.38s，闭包逐字节一致） |
| 峰值 RSS | 183MB |
| 摊销 | 0.46s/5 ≈ 0.09s/goal（非逐 goal 专属成本） |

GT 答案：g1=5（4 基图+1 派生）、g2=6（全派生）、g3=8（全基图，
**degenerate**，derived-only recall n/a）、g4=0（**空 GT，
degenerate**，recall/drecall n/a）、g5=2737（2273 基图+464 派生）。

## 总表（方法为行；wall=执行器 wall median；det/hyb 3 次 median，random 3 seeds median[min,max]）

| 方法 | g1 rec/wall | g2 rec/wall | g5 rec/wall | g5 drec |
|---|---|---|---|---|
| **chase** | 1.0 / 摊销0.09s | 1.0 | 1.0 | 1.0 |
| fwd null det | 1.0 / 0.001s | 1.0 / 0.783s [0.783,0.784] | 1.0 / 0.038s | 1.0 |
| fwd exact det | 1.0 / 0.001s | 1.0 / 2.641s [2.639,2.712] | 1.0 / 0.125s | 1.0 |
| fwd hdc det | 1.0 / 0.001s | 1.0 / 20.290s [20.279,20.301] | 1.0 / 0.798s | 1.0 |
| fwd * random×3 | recall 同 det | recall 同；wall 高于 det（null 约 +28%、exact 约 +9–12%、hdc 约 +1–2%） | recall 同；wall 高于 det（null 约 +25%、exact 约 +10–17%、hdc 约 +4–6%） | 1.0 |
| fwd * hyb | recall/wall 同 det（噪声内） | recall 同，wall 接近 det | recall 同，wall 接近 det | 1.0 |
| bwd score | 1.0 / 0.001s | 1.0 / 0.026s [0.025,0.026] | **0.991 / 0.012s** | **0.946** |
| bwd random×3 | 1.0 | 1.0 / 0.021–0.071s | 0.991（全 seed 同） | 0.946 |
| bwd hyb×3 | 1.0 | 1.0 / 0.021–0.070s | 0.991 | 0.946 |

（g3/g4 为 degenerate：全臂 all-answer recall 1.0（g3，答案全在基图）
或 n/a（g4 空 GT）；两 goal 各臂 precision=1.0、派生 0/151 条、毫秒级。
逐 run 明细见 detail64.tsv。）

## 结论

1. **policy 不改变本矩阵的最终 goal recall，但会改变搜索成本**：
   forward 九配置在非退化 goal 上均为 100%，backward 七配置也各自一致
   （g5 均为 99.09%）；random 的 wall/尝试 cost 可高于 deterministic，
   不能概括成“完全无差”。这与 1,652 规则上 places_lived 的排序敏感性
   形成规模对照。
2. **scorer 不改变本矩阵的 goal 答案，只改变搜索成本**：forward 三
   scorer 的 goal recall 一致；耗时 null : exact : hdc ≈ 1 : 3.4 : 26
   （g2：0.78s / 2.64s / 20.3s）——HDC 的 motif 级余弦打分是纯开销，
   零 goal-recall 收益（本次在无截断 GT 下确认）。
3. **forward vs backward 分野**：g2（6 个派生答案）backward 26ms/派生
   6 条 vs forward 0.78s+/派生约 4,656 条（forward fixpoint 会推导 DG
   锥内大量事实）——backward 在该小答案集上快约 30–100×；g5（464 个
   派生答案）**forward 完备（100%）而 backward 停在 99.09%/
   derived-only 94.61%**（fixpoint 早收：backward 候选空间结构性缺口，
   非深度截断——depth=64 未触及）。
4. **chase 与按需 agent 各有最快场景**：chase 一次 0.46s 覆盖全部 5 个
   goal，适合多 goal/完整闭包；但单 goal 可以低于 0.09s 摊销成本——g2
   backward 0.026s 完整、g5 forward+null 0.038s 完整。相反，g2 forward
   因推导大量 DG 锥事实而慢于 chase。没有单一 agent 配置在所有 goal 最优。
5. 无 timeout run；wall 全部为执行器主循环时间（median）。
6. 5 个 goal 中 g3/g4 按预注册保留但退化，主要非平凡证据来自 g1/g2/g5；
   不把这一小规则集的结果外推为所有规则规模的普遍规律。

## 复现

```bash
# 规则子集（确定性，可重建）
python3 experiments/full_policy_matrix/select_rules.py \
  /ssd_data/zhuxk/graphs/KG/fb15k-237/rules .work_hdcv/full_policy_matrix
# GT（fixpoint 门槛）
./build/bin/gar_reason --mode full --engine cpu \
  --csr /ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr \
  --rules .work_hdcv/full_policy_matrix/rules64 \
  --out .work_hdcv/full_policy_matrix/gt_closure64 --threads 1 --print 0 \
  --max-rounds 1000 --max-derivations 1000000000 --max-matches 1000000000 --no-matches
# 矩阵（串行、断点续跑）
bash experiments/full_policy_matrix/run_matrix64.sh
# 聚合
python3 experiments/full_policy_matrix/aggregate64.py
```

产物：`.work_hdcv/full_policy_matrix/{rules64,gt_closure64,runs64,
manifest.tsv,gt_answers64.tsv,detail64.tsv}`；本目录：NOTES.md /
RULE_SUBSET.md / select_rules.py / run_matrix.sh（1652 版，blocked
存档）/ run_matrix64.sh / aggregate64.py / detail64.tsv（见下 commit）。
