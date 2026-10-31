# forward_incremental：增量 forward greedy 替换 naive — 正确性 + 受控性能

日期：2026-10-31。预注册：NOTES.md（判据未移动）。实现 commit aee79d7 +
复核修复（见 NOTES"复核修正记录"：active 判定的 dynamic 重准入覆盖、
verify 扩展到全部 delta 触及规则、no-pick 的 key 生命周期、cap 语义精
确化；修复后 fi_perf 全 10 run 的 steps/derived/termination 逐数不变）。
计划：plans/phantom-stranger-rocket-phantom-stranger.md。

## 变更内容（对应计划逐步）

1. **规则级选择前置**：`ScoredRule`（rel_fam/confidence/price/priority，
   motif.h）+ `Scorer::ScoreRule`（exact/hdc 共享静态项，null=0）+
   `Policy::SelectRule`（deterministic/hybrid argmax、random softmax、
   Jev 规则级走确定性 fallback）。GreedyExecutor 每步：policy 选规则 →
   只匹配该规则 → 规则内 motif 级打分选择 → 应用一个。
2. **持久候选 cache + delta**：每规则一次 PatternSpec/head/label 预处理；
   首次选中才全量枚举（未选中规则零匹配）；cache 重过滤（已知结论剔除、
   动态失败按 version 重准入）代替重枚举；delta 用 PatternMatcher pins
   （新事实端点 pin 到每个同 slot pattern edge，含 mirror slot、多 edge
   同 slot 去重、goal pin 冲突即空集跳过）。goal pins 只用于 head 谓词
   == goal 谓词的规则（无多跳传播）。
3. **TripleGraph**：单 slot `AddEdges` 只更新 pair 数组+哈希、统一邻接
   标脏惰性重建（所有公开读取 Ensure 最新）；`AddEdgesBatch` 保持每轮
   一次急切重建（chase 语义不变）；四处 HasEdge 点查改 RelationPairSet
   哈希（O(1) 且始终最新）。
4. **WitnessLog**：HDC 证据仅 HdcScorer 维护（`NeedsHdcWitness`）；
   exact/null 跳过 8192 维编码，精确 witness 照常。
5. **统计**：summary.yaml/stderr 新增 fwd_rule_selections /
   fwd_rules_initialized / fwd_delta_enums / fwd_embeddings_generated /
   fwd_candidates_reused / fwd_capped_enums / fwd_match_wall_ms /
   fwd_apply_wall_ms / fwd_score_wall_ms。
6. **naive 删除**：NaiveMotifGenerator 移除；全候选枚举仅存为 bnb 专用
   `EnumerateAllCandidates`（bnb 证书语义不变）。无 --forward-engine
   回退。

## 正确性（预注册判据逐条）

| 判据 | 结果 |
|---|---|
| chase 回归 | `gar_reason --mode full`（fb15k 子集 2 轮）summary.yaml + closure_edges.tsv **与 .baseline/fb_deduce_cpu 逐字节一致** |
| bnb 回归 | synth bnb 链与既有基线逐字节一致 |
| backward 回归 | synth backward 链逐字节一致 |
| delta 等价 | GAR_VERIFY_INCREMENTAL=1 下：synth full-enum（含 inverse、重复事实）+ fb15k profession budget=300（match cap 100000）+ 接地 goal（cap 1000，goal pins）全部 rc=0 零 MISMATCH |
| 端到端 | synth 无 cap full-enum 答案集 {p0,p3,p6,p8} == chase 答案集；fb15k budget=10000 全 10 run 派生 ⊆ 30M 闭包零越界；--verify-against-closure pass；300s probe 的闭包外派生经前提抽样核验为闭包截断所致（见下"soundness 边界"） |
| scorer/policy 覆盖 | synth 上 hdc/null/random/hybrid 四组合答案集一致且正确 |
| 截断明示 | fwd_capped_enums 计数（cap64 各行 12–735 次）；profession cap64 recall 94.1% < cap100000 100% 即 cap 截断的可观测证据 |

## 性能（fb15k-237 × 1,652 规则，串行，deterministic+exact，budget=10000）

| goal（GT 答案数） | cap | recall | wall | 终止 | 历史 naive 对照 |
|---|---|---|---|---|---|
| award_nominations(5) | 64 | **100%** | **0.013s** | fixpoint | naive 同档（budget≤10）0 命中 |
| award_nominations(5) | 100k | 100% | 24.1s | budget | — |
| awards_won(6) | 64 | **100%** | **0.71s** | fixpoint | 同 |
| awards_won(6) | 100k | 100% | 3.9s | budget | — |
| profession(51) | 64 | 94.1% | 0.064s | fixpoint | — |
| profession(51) | 100k | **100%** | **0.115s** | fixpoint | backward 100%@20.2s（175×） |
| places_lived(243) | 64/100k | **0%** | 0.92s/4.0s | budget | backward score 臂同样 0%（结构性，非引擎差异） |
| film/language(2052) | 64 | 18.3% | 0.92s | budget(10k) | naive 11.3%@1800s timeout |
| film/language(2052) | 64 | **71.4%** | 300s | timeout(wall) | 同上；backward fixpoint 23.9%@7.5s |

probe：film/language cap=64 budget=1e9 wall=300s → 387,910 派生
（1,293 派生/s，naive 为 3.1 派生/s 的 ~420×），recall 爬升至 71.4%
仍未 fixpoint。places_lived 同配置 300s → 187,388 派生、**0 答案**
（与 backward score 臂 0% 同根因：该 goal 的 DG 邻域排序把两类引擎
都引进无效区域——是 policy/scorer 结构性问题，非引擎差异）。

**soundness 边界（如实）**：budget=10000 的 10 个 run 全部派生 ⊆ 30M
闭包（零越界）。300s probe 有 27,329/387,910（film）与
67,268/187,388（places）条在 30M 闭包**之外**——核验判定为**闭包
不完备而非引擎越界**：30M 闭包 summary 自报 `fixpoint: false`（1M
match cap 在 round≥5 截断）；抽样 nationality(/m/01zcrv,/m/07t21)
（gar_001478，链步 9091）两个前提分别在基图与 30M 闭包 round-4 中
——即 agent 的 pinned/delta 枚举达到了 chase 全量枚举被 cap 截掉的
合法 round-5 派生。**引擎的 soundness 保证是 EvalAntecedent 门控
（每步都是合法 chase 步），闭包对账在 chase 自身截断处失效。**

**判据核对**：
- 零 soundness 错误 ✓（全部派生 EvalAntecedent 门控 + 30M 闭包核验）
- 工作量大幅下降 ✓（例：profession 全运行仅 30 条规则初始化、
  12,305 embedding（cap100k）；naive 等价工作量 = 每步全 DG 可达规则
  ×步数。award_nom 接地 goal：1 规则初始化、1 embedding、1 步命中）
- 浅小 goal 端到端优于历史 naive ✓（0.013–0.71s 100% 完备 vs naive
  任何预算下从未 fixpoint、30min 仅 11% recall）
- 不宣称优于 chase：film/language 大答案集 chase 89.3s 完备，新引擎
  300s 仅 71.4%——**全外延大答案集仍以 chase 为基线** ✓

**新瓶颈（如实记录）**：cap=100000 时 per-step 全 cache 重打分主导
（award_nom score_ms=22.1s/24.1s wall）；cap=64 时该成本消失且更快
fixpoint。**cap 语义影响终止**：大 cap 下 attractor 规则 cache 难以耗
尽 → budget 耗尽而非 fixpoint；小 cap 反而更快到达 fixpoint（cap 是
截断参数，不是性能参数——文档已述）。

## 复现

```bash
cmake --build build -j 8
# 正确性（含 delta 校验模式）
GAR_VERIFY_INCREMENTAL=1 ./build/bin/gar_reason --mode agent --csr <csr> \
  --rules <rules> --goal '<rel>(_,B)' --executor greedy --forward-full-enum ...
# 性能（串行）
bash .work_hdcv/fi_perf.sh   # 5 goal × 2 cap，budget=10000
```

运行产物 `.work_hdcv/fi_perf/`、校验 `.work_hdcv/fi_verify_prof/`。
历史 naive 数字：experiments/forward_variants/ 与 open_full/
full_matrix_summary.md 的 fwd_* 行（已移除实现，仅基线）。
