# Learned relevance policy 负增益诊断（FB15k-237 / rules640）

日期：2026-11-21。问题：`--policy learned`（action-goal relevance model）相对
`--policy deterministic` 没有正增益，反而更慢、探索更多事实。本文记录从代码 + 数据
定位的三个根因、确认实验与最终结论。所有数字按 UNIFIED640 口径：36 test goals ×
3 seeds（7/13/42），`execution` 为 execution_cost_ms 均值（秒），`derived` 为派生事实
均值；基线 Deterministic = 2.39 s / 46047.6。

## 被诊断的对象

`core/engine/agent/src/learning/` 的 relevance model（不是旧 HDC-Q）：

- 模型：MLP（tanh 隐层 + sigmoid 输出），输入 `(HDC v_action, HDC v_goal, 手工分数 f)`。
- 标签：`y = γ^{d_C(μ,g)}`，即候选事实 μ 在 goal 证明 DAG 上到 goal 的距离的指数衰减
  （`proof_dataset.cpp` 从 goal 反向 BFS 证明前提得到距离）。正样本 = 证明 DAG 上的事实，
  负样本 = 非证明候选（每 goal ≤ 正样本 × negative_ratio）。
- 打分：`LearnedPolicy::Score = handcrafted_weight·f + relevance_weight·Predict`。
- CLI 默认（`tools/cli/agent_mode.h`）：`learned_handcrafted_weight=0`、
  `learned_relevance_weight=1`。

## 三个根因

### 根因一：训练数据稀缺，模型根本没学会（已实锤）

旧训练只有 10 个 train goals，正样本仅取证明 DAG 上的事实（每 goal 证明仅 ~3 事实），
故 `positive_examples=27 / train_examples=135`，对 ~8321 参数（dim128/hidden32）严重欠定。
`epoch_mse` 8 轮几乎不降（0.2507→0.2408 ≈ 平凡基线），`recall_at_1=0`，输出近常数，
argmax 退化为插入序。瓶颈是 **goal 数太少**，不是 trace 短（trace 共 658,878 条推导，
但绝大多数不在任何 goal 的证明 DAG 上，加长 trace 无用）。

确认：把训练 goal 扩到 135 个**可派生** goal（从 chase 闭包 `closure_edges.tsv` 按
round 2–4 哈希采样，70 个关系，排除 train/test 防泄漏），正样本升到 **6188**、训练样本
**7252**，`epoch_mse` 20 轮 **0.231→0.064**（−72%，单调下降）。模型这次真的学会了。

数据来源坑：`gengoals.tsv` 池全是基图已有事实（trace 全部 `goal_already_in_graph`、
0 推导），不能用作训练 goal；正确来源是只含派生事实的 chase 闭包。

### 根因二：默认权重丢弃手工分数 f（已实锤）

默认 `handcrafted_weight=0` 扔掉了 Deterministic 赖以排序的
`f = α·rel_fam + β·rel_evd − λ·dcost`。把 f 加回（`--learned-handcrafted-weight 1
--learned-relevance-weight 1`，blend11）即从纯 Learned 大幅回收：derived 48448.9→46160.6
（几乎追平 Det），execution 3.83→2.93 s。

### 根因三：标签不是「下一步价值」信号（已实锤，最终结论）

即便模型学会（根因一修复），端到端仍无正增益：

| 配置 | 模型 | 权重 (f / 模型) | execution (s) | derived | hit |
|---|---|---|---|---|---|
| Deterministic | — | — | **2.39** | **46047.6** | 1.0 |
| old Learned | 未训练 | 0 / 1 | 3.83 | 48448.9 | 1.0 |
| blend11 | 未训练 | 1 / 1 | 2.93 | 46160.6 | 1.0 |
| newpure | 已训练 | 0 / 1 | 3.43 | 49409.1 | 1.0 |
| newblend | 已训练 | 1 / 1 | 2.88 | 46125.0 | 1.0 |

- 已训练纯 learned（newpure）仍劣于 Det：derived +7.3%、execution +43%。
- 已训练 blend（newblend）≈ 未训练 blend（blend11）≈ Det（derived 46125 / 46160 / 46047）：
  学会的 proof-proximity 信号叠加在 f 上几乎零增益。

解释：`y=γ^{d_C(μ,g)}` 度量的是「事实在**最终证明**里离 goal 多近」，是证明结构的静态
属性，不是「**当前状态下**选该动作能减少多少剩余搜索」的边际价值；而且它与手工 f 的
`rel_fam=γ^{Σd(q)}`（family 距离）高度重叠，模型本质上在重新学一个 f 已经编码的、带噪声
的版本，自然学不出超越 f 的排序。pairwise accuracy 高 ≠ 更好的下一步排序。

## 追加验证：二值证明有用性标签（仍封顶 ≈ Det）

为检验「换个更干净的证明标签能否突破」，新增 `--label binary`
（`BuildBinaryProofDataset`，复用证明 BFS，目标改为 on-proof=1 / off-proof=0）。动机：
off-proof 动作正是 Deterministic 的浪费来源，学会压低它们*可能*减少探索。结果：

| 配置 | 标签 | 权重 (f / 模型) | execution (s) | derived |
|---|---|---|---|---|
| Deterministic | — | — | **2.39** | **46047.6** |
| newpure |  graded γ^d | 0 / 1 | 3.43 | 49409.1 |
| binpure | binary | 0 / 1 | 2.90 | 46287.0 |
| newblend | graded γ^d | 1 / 1 | 2.88 | 46125.0 |
| binblend | binary | 1 / 1 | 2.84 | 46137.8 |

- 二值标签显著优于 graded：纯 learned（binpure，**不用 f**）derived 46287 vs graded
  49409，几乎追平 Det（+0.52%）——说明 graded 距离回归引入了额外噪声。
- 但**所有臂仍不赢 Det**：binblend derived 46137.8 ≈ newblend 46125 ≈ Det 46047.6
  （均 +0.2% 内）；execution 各 blend 2.84–2.93 vs Det 2.39（+19–22%）。

强化结论：无论 graded 还是 binary，**证明结构标签都封顶 ≈ Det**。模型最多学会复现 f 的
排序，无法超越；derived 不降则 MLP 在线推理成本（每候选一次 Predict）成为纯开销，故更慢。
要正增益必须让 derived **低于** Det，即标签对齐「边际下一步价值/成本下降」，而非证明邻近度。

注：`--label binary` 与 `BuildBinaryProofDataset` 的代码改动落在 `learning/` 模块与
`gar_train_policy.cpp`（当前均为未跟踪的在途文件），随该模块一并由作者提交。

## 搜索头room：证明导向 backward 有 ~2000× 空间，但 53% 完备

`run_backward_headroom.py` 在 36 test goals 上跑 `--search-direction backward`
（`--backward-max-calls 2000000 --backward-max-depth 256 --wall-limit 60`）：

| 指标 | backward（命中 goal） | Deterministic |
|---|---|---|
| 命中率 | **19/36（53%）** | 36/36 |
| derived 均值 | **22.1 事实** | 46047.6 |
| execution 均值 | **4.3 ms** | 2.39 s |

- backward 是证明导向的：命中时仅推导 ~22 事实（Det ~46047，约 **2000×** 差距）、~4ms。
  例 p06 仅 3 事实命中。这说明「成本下降」的头room 极大——证明导向搜索能把探索量降到个位/十位。
- 但 17/36 失败，全部 `backward_call_limit`（中心谓词如 nationality 依赖锥巨大，反向子目标
  爆炸，2M call 仍不命中）。与「GoalChase 锥过滤对中心谓词收效甚微」一致。

**战略含义**：learned relevance policy 一直在试图*边际改进* forward Det 的排序（已证封顶
≈ Det），而真正的头room 在*证明导向*搜索（backward 式，2000× 降探索）。要拿到显著正增益，
方向应从「给 forward Det 学一个更好的排序」转向「让 forward 策略学会 backward 式的证明导向
行为」，并解决中心谓词的完备性（backward 失败时回退 forward）。

## Hybrid 实证：首个正增益（−18.9% 探索 / −14.2% 时间，100% 命中）

落地 `HybridExecutor`（`--search-direction hybrid`）：backward 优先（受限 call 预算，默认
100000，硬 goal 快速失败），失败则从 backward 留下的状态继续 forward AdaDeduce 到命中。
两 executor 都报告 `derived = state->journal()`（共享 journal），故 forward 的 derived 已含
backward 事实，合并时只拼 chain 与成本、不重复累加 derived。`run_hybrid_eval.py` 同脚本同
设置跑 36 goals × 3 seeds：

| 指标 | Hybrid | Forward（matched） | 增益 |
|---|---|---|---|
| hit | **1.0** | 1.0 | 持平（完备性保留） |
| derived | **37364.8** | 46047.6 | **−18.9%** |
| execution (s) | **2.098** | 2.445 | **−14.2%** |

- 这是所有臂里**第一个真正赢 Deterministic** 的：探索量与时间双降，命中不减。
- 机制：backward 解出的 ~19 goal 享受证明导向降探索（个位/十位事实），其余 17 个中心谓词
  goal 干净回退 forward（≈ Det 成本 + 少量 backward 开销）。平均下来净降 18.9%。
- 探针：p06 backward 命中 3 事实/1.2ms；p00 backward 失败→forward 回退命中 6236 事实/177ms。
- 可靠性：backward 与 forward 各自已证可靠，hybrid 仅在共享状态上顺序组合两者，可靠性保持。

注：hybrid 代码（`config.h` 加 `kHybrid`、`executor.h`/`hybrid_executor.cpp`、`CMakeLists.txt`、
`agent_mode.h` 选择器）与作者在这些核心文件上的在途改动交织，留工作树由作者一并提交。

### 增益来源分解与 backward-teacher 的排除

逐 goal 对比 hybrid vs forward（三 seed 中位数）：

| goal 类别 | 数量 | hybrid derived | forward derived |
|---|---|---|---|
| backward 命中（hybrid < 50% forward） | 19 | **23** | 16230 |
| 回退（hybrid ≥ 50% forward） | 17 | 79099.8 | 79372.6 |

- **增益 100% 来自 19 个简单 goal**（23 vs 16230）；17 个回退 goal（中心谓词/深层：round1×2、
  round2×7、deep×8）hybrid ≈ forward，无降。
- 据此**排除 backward-teacher 模仿**：①回退 goal 上 backward 全失败，无教师 trace；教师数据只能
  来自结构不同的简单 goal，难泛化。②简单 goal 已近最优（23 事实），无空间。③根因三特征天花板
  仍在——证明标签封顶 ≈ Det，换干净教师不改变 `(HDC action, HDC goal, f)` 的表达力上限。
- 结论：hybrid 增益是**结构性**的（backward 廉价解简单 goal），不是在硬 goal 上可学到的。
  最终采纳 hybrid 为增益方案，不再投入 backward-teacher。

## 最终结论与论文就绪结果

诊断闭环：learned relevance policy 负增益源于①数据稀缺②默认丢 f③证明标签≠下一步价值；
证明结构标签（graded/binary）学会后仍封顶 ≈ Det。**真正的增益来自证明导向 hybrid**：

| 方法（FB15k-237/rules640，36 goals×3 seeds） | hit | derived（宏平均） | execution（s，宏平均） |
|---|---|---|---|
| Deterministic（forward） | 1.0 | 46047.6 | 2.445 |
| Learned graded（纯模型） | 1.0 | 49409.1 | 3.43 |
| Learned binary（纯模型） | 1.0 | 46287.0 | 2.90 |
| Learned blend（f+模型） | 1.0 | ~46130 | 2.84–2.93 |
| **Hybrid（backward-first）** | **1.0** | **37364.8** | **2.098** |

Hybrid 相对 Deterministic：**derived −18.9%、execution −14.2%、命中持平**。
（论文 §6 表格可直接采用此行；paper.tex 未改动，由作者填入。）

## 修复方向（已定：采纳 hybrid）

头room 数据表明，边际改进 forward Det 已封顶；显著增益须转向证明导向。按性价比排序：

0. **证明导向 hybrid（首选，已验证正增益 −18.9%/−14.2%）**：backward 优先 + 中心谓词/超限
   回退 forward 的 `HybridExecutor` 已落地并实测赢 Det（见上节）。后续可把 backward 成功 trace
   当*更好教师* 教 forward learned policy 学会证明导向行为，进一步把回退 goal 的探索也降下来
   （需 backward trace 支持）。这是真正能大幅降 derived 的方向（不是边际调排序）。
1. **成本下降价值标签**：标签 = 选该动作后剩余搜索/证明成本的下降量（反事实 rollout 或信用
   分配），对齐「选它能省多少」。若以 backward 证明为参照分配信用，可与方向 0 结合。
2. **模仿学习 / learning-to-rank**：以参考策略每个决策点选中候选为正例、被拒绝 ready-set
   候选为负例。注意：现有 trace 事后从已执行步骤重建，**只记已采取动作（全 selected=true），
   不含被拒绝候选**——真行为克隆需先在选择时刻记录 ready-set 快照（改 deduce_session 选择
   路径 + trace 机制）。模仿 Det 封顶 ≈ Det（f 已是输入）；须模仿*更好*教师（如 backward）才有增益。
3. 保留 f 为主、模型只学 f 的**残差**（修正 f 的系统性偏差），而非取代 f。

主张 learned policy 优势前须过三关：数据充足（模型学会）→ 不丢 f → 标签是下一步价值。
当前证据：前两关已过，第三关证明结构标签（graded/binary）不满足，需证明导向/成本下降信号。

## 头room 与可学性：oracle 上界 + 两个去风险探针

`run_oracle_bound.py`（forward trace 的 derivations 恰为证明 DAG，len = 完美策略最小探索）：

| 分层 | derived（Det） | proof（oracle） | 浪费 |
|---|---|---|---|
| round1 | 7831 | 1 | 91.4% |
| round2 | 7435 | 3.75 | 91.3% |
| deep | 122877 | 43.6 | 98.2% |
| 总体 | 46048 | 16.1 | 93.6% |

逐 goal：p21 derived 575480 / proof 22，p34 356169 / proof 9。**证明锥处处很小、浪费处处
巨大，硬 goal 也有海量头room**——「硬 goal 证明锥本就巨大」的悲观假设被推翻。

但头room ≠ 可学。两个探针（`run_exploration_probe.py` 等）堵上了 obvious 学习路线：

- **探针 1（f 实例级 AUC）= 0.553**（≈随机）：f 谓词级近视实锤，同谓词实例几乎无区分力。
- **探针 2（探索 rollout）**：硬 goal random/uniform 多 seed，p09 最好 0.97×Det、p11 最好
  0.82×Det（幸运 seed，中位数常更差）、p21 最好 1.0×Det。**探索找不出数量级更省的证明**。

oracle 的 98% 头room 与探索最好 0.82× 之间的鸿沟**换排序跨不过**：浪费是实例级 brute-force，
forward 不管怎么排都得先推完错实例才撞见证明。唯一能吃头room 的机制是**直接认出证明实例**
（backward 常量绑定结构性做到，但硬 goal 反向爆炸失败）。

### 实例级特征探针：路径 A 证伪（gar_hdc_probe / gar_graph_probe）

「直接认出证明实例」需要实例级特征。两个一次性 C++ 探针（用真实 FactEncoder / Graph 出邻接）
测特征对证明成员身份的 AUC，按 goal round 分层（relevance_scale 133 traces，7252 候选）：

| 分层 | HDC 余弦 AUC | 多跳 BFS 图距离 AUC | f AUC |
|---|---|---|---|
| round2 | 0.733 | 0.888 | 0.688 |
| round3 | 0.716 | 0.916 | 0.529 |
| round4（深） | 0.442 | 0.537 | 0.529 |
| 总体 | 0.462 | 0.596 | 0.553 |

- 浅 goal：两种特征都有强信号（图距离 0.89–0.92 最强），强于 f。
- **深 goal（round4，占 80% 样本、正是 hybrid 回退的硬 goal）：HDC 0.44、图距离 0.54，均≈随机。**
- 机制：HDC 只捕 1-hop 实体重叠；深 goal 证明成员身份是**规则可派生性**（规则依赖+常量绑定），
  不是基图实体proximity，中间实体可离 goal 很远，BFS/PPR 量错对象。
- **路径 A 证伪**：没有任何已测实例级特征在硬 goal 上携带证明信号。信号存在处（浅 goal）正是
  hybrid 已赢处，信号消失处（深 goal）正是最需要增益处——实例级学习在实际增益上加不了东西。
  oracle 头room 需 backward 式结构搜索，非「学更好排序」可得。

## 最终收口（B/C，用户拍板）

- **B（主张立到 hybrid）**：论文增益主张立在 hybrid 结构增益（−18.9%/−14.2%，100% 命中）；
  learned relevance policy 降为**诚实消融/负面发现**——从成功链学 action relevance 封顶于手工
  分数，因为①训练链由手工策略生成（蒸馏天花板）②f 相关性谓词级、模型继承近视③探索给不出更优
  教师。这是个有证据、有机制洞察的结果，只是「learned」不再是增益主角。
- **C（learned 的实在落点）**：把 learned 改为 **goal 级策略选择器**（forward/backward/锥过滤），
  补 hybrid 的盲目先试 backward；36 goal 的 backward 成败标签现成。绕开蒸馏天花板、真学习含量，
  增益小但真。这是实例级排序证伪后，learned 唯一剩下的实在贡献（未实现，列为后续）。
- **A（实例级价值学习）已探索并证伪**：HDC 与多跳图距离探针证明深 goal 无实例级证明信号（见上节）。
  论文宜写成「已探索的负面结果」——头room 真实但非排序学习可得，需结构证明搜索。这本身是有价值
  的诚实发现，而非未验证的悬置前提。

论文表述建议见 `PAPER_FRAMING.md`（paper.tex 未改动，由作者采纳）。**不保留「learned relevance
policy 带来增益」的无证据主张。**

## 复现脚本

- `run_relevance_scale.py`：从闭包采样可派生 goal、生成 deterministic trace、重训并打印
  报告（`--goals/--hidden/--epochs/--min-round/--max-round`）。
- `run_blend_ablation.py`：36 test goals × 3 seeds 端到端评测，`--handcrafted/--relevance`
  设权重、`--model` 指定模型、`--tag` 区分实验臂。
- `run_backward_headroom.py`：36 test goals 上跑 backward，统计命中率/探索量，量化证明导向
  搜索的头room（`--wall-limit/--max-calls/--limit`）。
- `run_hybrid_eval.py`：36 goals × 3 seeds 评测某 search-direction（hybrid/forward/backward），
  `--direction/--tag/--backward-max-calls`，输出 execution/derived/hit 汇总。
- `run_oracle_bound.py`：36 test goals 的证明 DAG 大小（oracle 最小探索）vs Det derived，
  量化可学头room（`--limit/--wall-limit`）。
- `run_exploration_probe.py`：硬 goal 上 random/uniform-random 多 seed，检验探索能否找到比
  Det 更省的证明（`--goals/--seeds/--policies/--wall-limit`）。

```bash
# 扩数据重训（确认根因一）
python3 experiments/fixpoint_policy/run_relevance_scale.py --goals 150 --hidden 32 --epochs 20
# 端到端对照（确认根因二/三）
M=.work_hdcv/relevance_scale/model_h32_now/relevance.meta
python3 experiments/fixpoint_policy/run_blend_ablation.py --handcrafted 0 --relevance 1 --tag newpure  --model $M
python3 experiments/fixpoint_policy/run_blend_ablation.py --handcrafted 1 --relevance 1 --tag newblend --model $M
# 二值证明有用性标签（追加验证）
./build/bin/gar_train_policy --csr /ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr \
  --rules .work_hdcv/fixpoint_rules640/rules640 --traces .work_hdcv/relevance_scale/traces \
  --test-goals .work_hdcv/fixpoint_q640/test_goals.tsv \
  --out .work_hdcv/relevance_scale/model_binary/relevance.meta --label binary \
  --epochs 20 --batch-size 64 --hidden 32 --negative-ratio 4 --seed 42
B=.work_hdcv/relevance_scale/model_binary/relevance.meta
python3 experiments/fixpoint_policy/run_blend_ablation.py --handcrafted 0 --relevance 1 --tag binpure  --model $B
python3 experiments/fixpoint_policy/run_blend_ablation.py --handcrafted 1 --relevance 1 --tag binblend --model $B
# 证明导向搜索头room（backward vs forward Det）
python3 experiments/fixpoint_policy/run_backward_headroom.py --wall-limit 60
# Hybrid 实证（首个正增益）+ matched forward 基线
python3 experiments/fixpoint_policy/run_hybrid_eval.py --direction hybrid  --tag hybrid
python3 experiments/fixpoint_policy/run_hybrid_eval.py --direction forward --tag forward
# 头room 与可学性（oracle 上界 + 探索探针）
python3 experiments/fixpoint_policy/run_oracle_bound.py
python3 experiments/fixpoint_policy/run_exploration_probe.py --goals p09,p11,p21,p07
```