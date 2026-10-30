# full_matrix：大答案集完整 baseline 矩阵（budget=1e9 + 30min 墙钟）

日期：2026-10-30。预注册：FULL_MATRIX_NOTES.md。最小扩展 commit
30aa28b（--forward-full-enum + --wall-limit，默认逐字节回归：synth
bnb/backward 链 diff 一致）。

**Setting**：fb15k-237 全基图 × 1,652 规则子集；4 个大答案集 goal；
GT = 基图 ∪ capped chase（10M/4 轮）；全部臂 budget=1e9、执行器内
wall-limit 1800s（干净终止、部分结果照常落盘）、match cap 100000
（backward 加 collect cap 1e7）。**chase 基线：89.3s、4/4 完备**。

## 汇总表（方法为行；recall 以 chase=1 为满分）

| 方法 | 平均 recall | 平均 wall | 终止形态（4 goal） |
|---|---|---|---|
| **forward chase（基线）** | **1.000** | **89.3s** | 一次运行全覆盖 |
| backward + score | 0.221 | 569s | 3 fixpoint + 1 timeout |
| backward + random(s42) | 0.221 | 570s | 3 fixpoint + 1 timeout |
| backward + hybrid | 0.221 | 568s | 3 fixpoint + 1 timeout |
| backward + HDC(512) | 0.221 | 570s | 3 fixpoint + 1 timeout |
| forward greedy + exact | 0.116 | 1800s | **4 timeout** |
| forward greedy + HDC(512) | 0.095 | 1800s | **4 timeout** |
| forward bnb utility / hdc-value | N/A | — | 单链最优证书语义，不回答全外延（见文末） |

## 逐 goal recall

| arm | film/language(2,052) | position(9,624) | release_region(857k) | contains(1.49M) |
|---|---|---|---|---|
| backward × 4 臂 | 0.239（全同） | 0.559（全同） | 0.038（全同） | 0.049–0.050 |
| fwd greedy exact | 0.113 | 0.329 | 0.018 | 0.004 |
| fwd greedy HDC | 0.072 | 0.288 | 0.018 | 0.004 |
| **chase** | **1.0** | **1.0** | **1.0** | **1.0** |

backward 各 goal wall：film/language 7.5s、position 66s、
release_region ~450s（均 fixpoint）、contains 1800s timeout
（~7.3 万/149 万）。forward greedy 四 goal 全部 1800s timeout。

## 结论

**① forward greedy 全枚举（核心新数据）：大答案集上全面劣于
backward，且两者都远不及一次 chase。** 四 goal 全部打满 30min 未
fixpoint，recall 0.4%–33%；对照 backward 同 goal 在 7.5s–450s 内
fixpoint 且 recall 更高（position：fwd 33%@1800s vs bwd 56%@66s）。
机理（film/language 剖析）：greedy 每步 0.28s（全规则候选重生成，
cap=100000），且 scorer 把它引进 award 吸引子——journal 5,594 条
中仅 945 条 film/language（其中答案 232 条，**4.1% 的派生在答案
上**），3,376 条是 award_nominated/award_winning 吸引子事实。
forward greedy 的"指导"在全外延任务上变成系统性偏离。

**② HDC 两臂均符合预注册的无增益预期。**
- backward + HDC：recall/journal 与 score 臂**逐数相同**（fixpoint
  由结构决定，排序不影响完备性），wall 持平——HDC 读数对全外延
  既无增益也无（明显）减速。
- forward + HDC：recall 全面更低（0.095 vs 0.116）且每步 ~4× 慢
  （同 30min 仅 1,428 vs 5,687 平均步数）——HDC 打分成本直接吞噬
  枚举预算。

**③ backward 排序臂（score/random/hybrid）**：大答案集上完全一致
（recall 最大差 0.001）——与 open_full 小/中层结论互补：排序只在
特定中层 goal（places_lived）分化，大层 fixpoint/timeout 由候选空
间结构决定。

**④ 大答案集的终极格局**：chase 一次 89.3s 完备（1.0） vs 最好的
agent 臂（backward）0.221 平均、最快单 goal 也要 7.5s 且只有 24%
——**答案集 ≥ 万级时全量 chase 是唯一可行全外延路径**（open_full
结论在无限预算下依然成立：瓶颈不是预算，是逐答案的应用成本与
backward 候选空间的结构性覆盖缺口，如 release_region fixpoint 于
3.8%）。

## bnb 臂为何 N/A

forward bnb（utility / hdc-value）是**单链最优证书**语义：在预算内
找一条 cost 最小的 goal 证明链并给最优性证书（Theorem 5），回答
"存不存在一条便宜证明"，不是"答案集的全外延"。以 1e9 预算跑 bnb
只会重复证明已到手的答案前缀，不产生外延语义，故标 N/A 不跑。

## soundness 与回归

- 全部派生经 EvalAntecedent 门控；**backward 三 goal 全部 64,204
  条派生 ⊆ 30M 高帽闭包，零越界**（含 release_region 18,120 条
  beyond-10M-GT）。
- --forward-full-enum / --wall-limit 默认关闭，synth bnb/backward
  链逐字节回归通过。
- --wall-limit 机制验证：timeout 终止照常写 summary/chain/closure
  （20 个 timeout run 数据完整）。

## 复现

```bash
bash .work_hdcv/fm_arm.sh           # 24 runs: 4 goals x 6 arms
python3 .work_hdcv/fm_aggregate.py  # → .work_hdcv/fm_detail.tsv
```

明细：`experiments/open_full/full_matrix_detail.tsv`；运行产物
`.work_hdcv/fm_runs/`。
