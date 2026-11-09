# hdc_hard_goals：实验 B —— 困难固定实例证明搜索（结果）

日期：2026-11-01。预注册：NOTES.md（冻结 cohort/训练隔离/判据，未移动）。
设置与 hdc_online 主 A/B 完全一致：fb15k-237 全图 × **1,652 规则**
（DG arcs=3023）、budget=2、cap=64、hybrid/exact、top_k=0、串行。
**本实验目录未做 git commit（用户要求保留工作区）。**

## 冻结 cohort 与训练构成

4 个锚点 goal（含负面对照 athletes），spec 逐条从原始 trace 核对：
- r2c_2 `/location/hud_county_place/county(/m/0r540,/m/0l2rj)`
- r2c_4 `/music/genre/artists(/m/0fd3y,/m/01vvycq)`
- r2c_8 `/film/film/runtime./film/film_cut/film_release_region(/m/04v8x9,/m/02w7gg)`
- athletes `/olympics/olympic_participating_country/athletes./olympics/olympic_athlete_affiliation/olympics(/m/05r4w,/m/09n48)`

新增 goal：**pending**（需按预注册的普通-BNB 可观测困难特征选取且不得
进入对应训练集；本时间盒内不可靠补齐，不伪造）。

训练隔离（两臂同一份 trace、同一 LOGO）：
- HDC 臂：复用 hdc_online 的 LOGO 模型（.work_hdcv/models/{g0,r2c_2,
  r2c_4,r2c_8}.meta；trained_on 见各 meta，测试 goal 不在其训练集）。
- numeric 臂：train_numeric.py 重训（同 trace、同标签：最优路径
  y=best−cost、其余 3.0；失败 trace 全 3.0），标准化均值/方差逐折在
  各自训练行上拟合（写入 meta）。train_nodes 与 HDC 臂逐折一致
  （33,377–33,678）——同一批训练行，唯一差异是特征表示（数值 16 维
  vs HDC 4096 维随机特征）。

## 主表（16/16 run 完成，串行；BNB 三臂同 binary 同 workload）

| goal | arm | 展开数 | wall(s) | hit | best_cost | cert | chain |
|---|---|---|---|---|---|---|---|
| r2c_2 | utility | 18,962 | 1325.8 | ✓ | 2 | ✓ | 基准 |
| r2c_2 | hdc-value | 1,326 (**-93.0%**) | 89.6 | ✓ | 2 | ✓ | 逐字节同 |
| r2c_2 | numeric | **1,254 (-93.4%)** | 85.2 | ✓ | 2 | ✓ | 逐字节同 |
| r2c_4 | utility | 12,791 | 348.9 | ✓ | 2 | ✓ | 基准 |
| r2c_4 | hdc-value | 897 (-93.0%) | 26.6 | ✓ | 2 | ✓ | 逐字节同 |
| r2c_4 | numeric | **710 (-94.4%)** | 21.5 | ✓ | 2 | ✓ | **异优链（注 1）** |
| r2c_8 | utility | 68 | 2.2 | ✓ | 2 | ✓ | 基准 |
| r2c_8 | hdc-value | 47 (-30.9%) | 1.7 | ✓ | 2 | ✓ | 逐字节同 |
| r2c_8 | numeric | 120 (**+76.5% 负**) | 3.5 | ✓ | 2 | ✓ | 逐字节同 |
| athletes | utility | 2,084 | 146.1 | ✓ | 2 | ✓ | 基准 |
| athletes | hdc-value | 2,058 (-1.2%) | 147.0 | ✓ | 2 | ✓ | 逐字节同 |
| athletes | numeric | **353 (-83.1%)** | 24.0 | ✓ | 2 | ✓ | 逐字节同 |
| **合计** | utility 33,905 | hdc 4,328 (**-87.2%**) | **numeric 2,437 (-92.8%)** | | | | |

注 1（r2c_4 numeric 链）：与 utility/hdc 同 facts、同 best_cost=2、同
certified，但首步用 gar_001310（peers 正向）而非 gar_001296（peers^-1
逆向）——等价的替代最优证明（两规则近重复，派生同一中间事实）。
证书语义不受影响；如实记录为非逐字节一致。

**backward-deterministic 参照**（budget=2）：4 goal 全命中、秒级完成
（wall<0.1s），**proof_cost=2**（chain.tsv 末行 cum_cost，4/4 与 BNB
的最优值一致）。成本口径（2026-11-01 更正，撤回此前"backward
best_cost=3 非最优"的错误结论）：backward 的 summary `total_cost`
是**搜索计费 search_spent**（含失败应用：r2c_2=3、athletes=3、
r2c_4/r2c_8=2），不是证明成本；proof_cost 一律从 chain.tsv 的
cum_cost 读取。CLI `--budget 2` 只是各执行器各自记账口径的上限，
**不等于各执行器预算记账语义相同**——不再使用"同口径成本"表述。
certified 对 backward 记 N/A（BNB 专有）。expansions 对
backward/chase 填 N/A。
**chase**：N/A——1,652 规则 uncapped fixpoint 已证实资源包络不可行
（full_policy_matrix 门槛证据：round-4 +59.0M、threads=8 round-5 RSS
312GB 逼近红线），不重复危险实验、不借 rules512 数字。expansions 对
chase 填 N/A。

## 正确性与资源检查

- **utility 臂复现**：r2c_2 18,962、r2c_4 12,791、r2c_8 68、athletes
  2,084——与 hdc_online 历史展开数**逐数一致**（当前 binary 的 BNB 路
  径语义未变；wall 因机器状态不同而更低，不作跨日加速比）。
- **HDC 臂复现**：r2c_2 1,326、r2c_4 897、r2c_8 47、athletes 2,058——
  与历史逐数一致（模型与机制未变）。
- best_cost=2 与 certified_optimal 全臂全 goal 一致；4 goal 中 3 个
  chain.tsv 三臂逐字节一致，r2c_4 numeric 为等价替代最优链（注 1）。
- soundness：16 run 全部派生 ⊆ 30M chase 闭包（.work_hdcv/
  of_chase30m），**零越界**。
- 模型推理开销：HDC 17.3–18.5µs/call（合计 ≤36ms/run）；numeric
  **0.32–0.35µs/call（~50× 更便宜，合计 ≤0.4ms/run）**——两者均可忽略。
- 默认路径回归：synth bnb utility chain 逐字节一致；格式守卫拒绝
  hdc 模型用于 numeric-value 臂（反向亦然）。
- numeric 数值一致性：C++ 预测 vs Python 参考 max|diff| = 1.8–3.6e-12
  （<1e-9，verify_numeric.py，4 折全过）。
- 峰值内存：单 run ≤ 13GB（串行）；看门狗零触发；全部产物在
  .work_hdcv/hdc_hard_goals/。

## 结论（锚点复核 + 新增评估 goal 泛化；各自分表，不混样本）

**A. 锚点复核（4 goal，见上主表）**：

1. **HDC-value 主 A/B 复核通过**（沿用预注册判据）：合计展开 -87.2%、
   wall 大幅下降、正确性全绿；athletes 负面对照持平（-1.2%）如期复现。
2. **收益不专属于 HDC 表示**——数值 ridge（同特征、同交互、同标签、
   同 LOGO）合计 -92.8%，且在每个 goal 上不低于 HDC（r2c_2 -93.4 vs
   -93.0、r2c_4 -94.4 vs -93.0、athletes **-83.1 vs -1.2**），仅在
   r2c_8 上为负（+76.5%，HDC 该 goal 唯一胜出）。athletes 上的巨大反
   差（HDC 持平、numeric -83%）尤其说明：瓶颈是 HDC 随机特征对该
   goal 族的表达能力，不是价值学习本身。
3. 工程推论：numeric 模型推理便宜 ~50×、模型文件 128B（vs HDC 33KB
   向量 + 32KB 权重）、无哈希种子依赖——若论文采用 value-ordering
   加速，**数值 ridge 应是首选基线**；HDC 版本的唯一优势（r2c_8 型小
   树目标）需要更大 cohort 才能判断是否稳健。
4. backward-deterministic：4 goal 全命中、proof_cost=2 与 BNB 最优值
   一致（链可与 BNB 不同——合法替代最优证明），但**不给最优性证书**
   （certified N/A）——BNB 的证书价值独立于排序加速。

**B. 新增评估 goal（g9/g22/g6，曾属旧训练池，逐折 LOGO 重训）——
易例对照（proof_cost=1，不满足困难定义，如实归位）**：

冻结依据与难度声明见 NOTES_NEW_GOALS.md（**非困难实例**：best_cost=1、
utility 展开 265/63/13，含中/低困难对照；谓词族隔离因样本不足判
N/A）。两臂（HDC/numeric）均按新测试 goal 剔除其成功 trace 重训
（同训练行/标签；train_nodes 33,568–33,694；trained_on 见各 meta）。

| goal | utility 展开 | hdc-value | numeric | 结论 |
|---|---|---|---|---|
| g9 | 265 / 10.5s | 265 / 10.6s (+0%) | 265 / 10.6s (+0%) | 无分化 |
| g22 | 63 / 4.3s | 63 / 4.3s (+0%) | 63 / 4.3s (+0%) | 无分化 |
| g6 | 13 / 0.6s | 13 / 0.6s (+0%) | 13 / 0.6s (+0%) | 无分化 |

- 正确性：三臂 chain.tsv 逐字节一致、best_cost=1、certified 全一致、
  soundness 零越界；backward 全命中、proof_cost=1（链同，spend=1）。
- **结论：在非困难实例上 value 排序无害也无益**——搜索树小到排序
  还来不及起作用（最优证明在排序生效前即被证出）。这与 r2c_8 的
  numeric 负例共同界定收益边界：**收益需要足够大的分支空间**；在
  此之下两臂与 utility 无法区分。
- 样本规模如实：易例对照仅 3 个 goal 且难度分层偏易（全部
  proof_cost=1）；**不满足 EXPERIMENT_PLAN §2 的困难实例定义
  （链长 2、明显失败分支、大搜索空间），不计入困难泛化证据**。

**C. 真正困难 goal 泛化（proof_cost=2 且 utility 展开 ≥1000）：
PENDING / 未验证**

预注册：NOTES_HARD_GENERALIZATION.md（第一轮降序）+
NOTES_HARD_FEASIBLE.md（第二轮升序，可行性门槛）——候选生成/筛选算
法均先冻结后执行，未依据 HDC/numeric 结果挑选。两轮执行与结果：

- 候选池：1652 规则 30M 闭包中 round==2 事实，经难度代理筛（生成规
  则 embeddings≥1000 + head 谓词 ≥5 条规则 + 排除全部旧 trace goal
  /谓词/tasks50/锚点/易例）→ **243,068** 个候选
  （`candidates_pool.tsv`；生成脚本已固化为
  experiments/hdc_hard_goals/make_hard_candidates.py，复跑产物与冻结
  件逐字节一致）。
- 第一轮（降序，池顶最难；manifest=candidates_manifest.tsv）：
  - 候选 1 `film_release_region(/m/02r_pp,/m/0b90_r)`（gar_000156）：
    2000s 外部 timeout，28,640 展开未 certify；
  - 候选 2 `film_release_region(/m/03np63f,/m/06q1r)`（gar_000156）：
    2000s 外部 timeout，28,608 展开未 certify。
- 第二轮（升序，自 1070-embedding 规则起；manifest=
  candidates_feasible_manifest.tsv；2026-11-04 断点恢复整理）：
  - f1 `nominated_for(/m/014kq6,/m/01npcx)`（gar_000202）：外部
    timeout 产物（约 2000s；日志无精确 wall，如实不伪造），末态
    **expanded=74,400、open=320、best_cost=-1**（goal 从未到达），
    无 summary.yaml；
  - f2 `nominated_for(/m/014kq6,/m/025twgf)`（gar_000202）：同上，
    **expanded=74,880、open=319、best_cost=-1**。
  f1+f2 累计 ~66min > 预注册 60min 确认预算 → 按协议停止，不再开新
  队列。
- 判定：**hard-generalization = PENDING/未验证**（两轮 0/4 通过）。
  机制记录：budget=2 的 cost-2 证书要求穷尽 cost+price<2 的全部 cost-1
  前沿分支；在本图/规则集上此前沿达数万至 7 万+，超出 2000s 包络——
  四个候选的困难性均被证实（展开数超门槛 28–74 倍），但**池内未发现
  包络内可认证实例**。注意 best_cost=-1 意味着这两轮甚至未取得
  incumbent——不能记为"不可证"，只能记"包络内未完成搜索"。
- 两轮四候选均未运行 HDC/numeric（协议：utility 基线先行）；
  锚点/易例模型未动。
- 解除路径（需显式授权，均未执行）：①加大单 run 包络 >2000s；
  ②重开预注册改排序键；③引擎级认证加速。

## 复现

```bash
python3 experiments/hdc_hard_goals/train_numeric.py        # numeric LOGO 模型
python3 experiments/hdc_hard_goals/train_new_goals.py      # 新增 goal 双格式 LOGO 模型
cmake --build build -j 8                                    # 含 value_model_check
python3 experiments/hdc_hard_goals/verify_numeric.py \
  .work_hdcv/hdc_hard_goals/models/r2c_2.meta               # C++/Python 一致性
bash .work_hdcv/hdc_hard_goals/run_expb.sh                  # 锚点主 A/B（断点续跑）
bash .work_hdcv/hdc_hard_goals/run_new_goals.sh             # 新增 goal A/B
python3 .work_hdcv/hdc_hard_goals/aggregate.py              # 汇总表（proof_cost 口径）
```
