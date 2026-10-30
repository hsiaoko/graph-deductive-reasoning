# open_goals：开放变元 goal 的方向实验

日期：2026-10-30。预注册：docs/direction/open_goal_experiment.md（commit
1092db9）。资产与过程见 NOTES.md。ground truth = 基图 ∪ capped chase
（2 轮/100k/30M，23,483,693 派生事实，~215s 一次覆盖全部 goal）。
hold-out：G0/G3 逐任务留出；G1/G2 开放 goal 求外延不删边。
最小扩展 `--open-collect-cap`（默认 8 逐字节不变）。

## G0（接地对照，Overall 复用）

backward 46/50 命中、mean 1.62s median 0.25s vs chase 摊销 215s/50=4.3s。
（结论沿用：接地 goal backward 数量级占优。）

## G1（单变元 r(x,B)，10 个，按答案集分层）

| 层 | 答案集 | backward 答案完备率 | backward wall | forward（一次闭包全量） |
|---|---|---|---|---|
| 小（1,2,7,9） | 1-9 | 100%,50%,33%,14% | 0.10-1.9s（一个 35s） | 全量，~215s 一次 |
| 中（43,50,61） | 43-61 | 0%,2%,12% | 0.65-7.0s（1 个 102s 超时） | 同上 |
| 大（1789,2034,2040） | ~2k | 0.1-0.3% | 0.18-1.7s | 同上 |

- **collect cap 不是瓶颈**：cap=8 与 cap=100 答案数完全一致——约束在
  budget（每答案=应用数，budget=10 只允许 ~10 步）。"time-to-first"
  快（0.04-0.67s 起步），"time-to-full-answer" 在小集内 budget 可达成，
  中/大集 budget 内不可能（成本=答案数）。
- **case 1 成立（大答案集）**：大/中答案集上 forward 一次闭包是全外延的
  唯一可行路径；backward 只给前缀。**case 1 的细化**：小答案集（<10）
  backward 仍更快（time-to-first ≈ 毫秒-秒级 vs 一次 215s chase）——
  backward 适用边界比 README 写的宽一档。

## G2（双变元 r(x,y)，5 个按 fan-in）

| fan-in | 谓词 | 答案（budget=10 内） | wall |
|---|---|---|---|
| 4943 | contains | 0 | 0.11s |
| 2554 | film_release_region | 2 | 0.15s |
| 226 | position | 2 | 0.06s |
| 2 | languages_spoken | 2 | 0.07s |
| 5 | film_art_direction_by | 1 | 0.04s |

全开放下 backward 只产零星前缀（<0.2s 内即终止）；与 fan-in 无可见关系。

## G3（hub/非 hub 对照，同谓词同 dst，逐任务留出）

| 谓词 | hub（deg）spent/wall | ctrl（deg）spent/wall | 比 |
|---|---|---|---|
| contains | 1 / 0.045s (5476) | 7 / 25.7s (177) | ctrl 571× |
| film | 1 / 0.039s (4957) | 10 MISS / 69.5s (354) | ctrl 失败 |
| exported_to | 1 / 0.038s (4913) | 2 / 1.8s (238) | ctrl 46× |
| currency | 1 / 0.040s (4849) | 3 / 24.9s (391) | ctrl 620× |
| currency | 1 / 0.040s (4482) | 2 / 13.5s (273) | ctrl 338× |

**hub 假设被反转**：grounded hub goal 全部一击即中（~40ms），非 hub 对照
贵 27-620× 且一个直接失败。机理：hub 目标边多基图或一步可达；非 hub
需深链。README 的 case 4 表述（"hub 削弱 pin 剪枝，forward 更有韧性"）
**应修正**：对 grounded goal，pin 在目标边上时 hub 反而更有利（fan-out
只在中间子目标处伤 backward；hub 的子目标反而更容易一步落地）。

## 混合原型（hybrid-prototype，如实标注）

DG 反向可达规则子集（15 个 goal 谓词）：深度 1 = 72,237/81,683 条
（88.4%），深度 2 = 100%——DG 过稠。chase 218s vs 纯 forward 215s，
**无收益，按预注册预警退化为纯 forward**。Magic-Sets 式常量约束子集
（非谓词级）需要真正按 goal 常量改写规则，超出原型范围。

## 判据逐条核对

1. case 1（大答案集 forward 反超）：**成立**（中/大集 forward 唯一全解）；
   细化：小集 backward 仍更快（边界比预期宽）。
2. backward 边界更宽：**部分成立**（小集成立；大集 budget 即答案数上限）。
3. G3 hub >10× 坐实 limitation：**不成立**——hub 反而快 27-620×，
   ctrl 1 个失败；建议删除该 limitation 候选并改写 case 4。
4. 混合双优：**不成立**（DG 子集 88%→无收益，退化纯 forward）。
5. soundness：所有派生经 EvalAntecedent 门控（合法 chase 步），
   零真实 DIVERGENT（verify 口径沿用既有执行器）。

## 对 docs/direction/README.md 的修正建议

- case 1 加注：答案集 <10 时 backward 仍占优（时间粒度差异）；
  ≥数十时 forward 是全解唯一路径。
- case 4 改写：hub 对 grounded goal 是优势而非限制（fan-out 伤的是
  中间子目标的稀疏性，hub 反而受益）；hub 的真正风险场景是 hub 作为
  开放中间变量（G2 全开放或深层协调链），不在本实验覆盖内，另行验证。

## 复现

- backward（变元）：`gar_reason --mode agent --goal 'rel(_,dst)' --executor
  backward --budget 10 --max-candidates-per-rule 8 [--open-collect-cap N]`
- G3：back_multi（.work_hdcv/back_multi.cpp，逐任务留出）
- ground truth：`gar_reason --mode full --max-rounds 2 --max-matches
  100000 --max-derivations 30000000 --threads 8`
- 混合：.work_hdcv/hybrid_rules（DG 反向深度 1 子集，符号链接目录）
