# HDC 价值模型：bnb 的学习型排序（新 HDC 的运作方式）

本文说明当前系统里**唯一有正向证据的 HDC 用法**：HDC 随机特征 + ridge
学习 bnb 分支的剩余成本（cost-to-go），作为 best-first 的可选优先级。
在线 A/B 判据全过（展开数 −87.2%、耗时 −86.4%、正确性不变），可进论文主线。
实现：`engine/agent/value_model.{h,cpp}`（commit `1f5a0f6`）。

## 1. 角色定位：不替代任何精确计算

| 工作 | 谁做 |
|---|---|
| DG 距离、见证比、conf、price | 原有精确模块（不变） |
| bound、剪枝、certified 证书 | bnb 原有逻辑（不变） |
| 派生事实 | 精确模式匹配 + EvalAntecedent（不变） |
| **预测"这条分支还要几步到 goal"，决定先展开谁** | **HDC 价值模型（新增，可选）** |

与已被实验否定的 HDC 角色（证据余弦打分、filter、经验泛化记忆）的本质
区别：那些角色是让 HDC **近似精确量**（精确方法又准又快，必输）；这里是
学习一个**没有精确方法可算的统计量**（剩余成本只能跑完搜索才知道）。
预测错了只是顺序笨一点，不会推错事实——soundness 与最优性证书不受影响。

## 2. 训练（离线，一次性）

### 2.1 数据采集：让 bnb 裸跑，日志即教材

```bash
GAR_DEBUG_BNB=1 ./build/bin/gar_reason --mode agent --executor bnb \
    --csr <csr> --rules <rules> --goal 'rel(s,d)' --budget 2 \
    --max-candidates-per-rule 64 --out <dir> > goal.log 2>&1
```

日志事件（`[bnbl]`）：`fork`（分支创建，含特征）、`eval`（优先级求值）、
`expand`（展开）、`goal`（找到解）、`discard`。事后回看得到标签：
最优链上的节点 → 真实剩余成本；off-path / 烧光预算的节点 → 失败标签。
无需人工标注。参考规模：7 个成功 goal + 40 个失败 goal ≈ 32.5k 训练节点。

### 2.2 特征与 HDC 编码

**HDC 特征表**（11 个状态特征，全部来自既有精确模块；除 cost/clen/wit
为分支级外，其余取 line-6 重排后**最优候选 motif** 的 scorer 分量，
见 `branch_bound_executor.cpp` 的 eval 日志与 ValueModel 调用处）：

| # | 特征 | 含义 |
|---|---|---|
| 0 | `cost` | 分支已花成本（unit cost 累计） |
| 1 | `clen` | 父分支推导链长度（本分支链长 −1，与训练口径一致） |
| 2 | `ncand` | line-6 截断后的候选 motif 数 |
| 3 | `minprice` | 最优候选的 price（= w₀ + d(p₀)） |
| 4 | `relfam` | 最优候选的 DG 距离相关性 γ^{Σd} |
| 5 | `relevd` | 最优候选的见证证据（已见证前提/总前提） |
| 6 | `conf` | 最优候选规则的挖掘置信度 |
| 7 | `dcost` | 最优候选的成本惩罚项 |
| 8 | `total` | 最优候选的原 scorer 总分 |
| 9 | `dp0` | 最优候选结论谓词到 goal 的 DG 距离 d(p₀) |
| 10 | `wit` | 分支见证日志的事实总数 |

- **分箱**：`bin = min(7, floor(|x| × 2))`，即 **0~7 共 8 档**。
- **随机向量分配**：每个 (特征, 档位) 分配一个 D=4096 维随机双极向量
  （int8）——共 11×8 = 88 个，加 1 个 bias 向量 = **89 个**，全部定种子
  生成，训练与在线一致。
- **合取特征（4 组，bind 逐维相乘）**：`cost×dp0`、`ncand×dcost`、
  `conf×total`、`cost×clen`，使模型能表达"距离近但见证少"这类组合
  规律——这是它优于纯线性工程特征的关键。
- **bundle 成一条 4096 维分支指纹**（11 个特征向量 + 4 个合取项 +
  bias 全部逐维相加，无拼接）→ **ridge（λ=1.0）回归剩余成本**。
- **导出**：`meta.txt`（编码参数 + 89 个 int8 随机向量）+
  `weights.bin`（4097 个 float64：4096 维权重 + 偏置）。
  训练脚本：`experiments/hdc_online/train_export.py`（需 numpy；
  与 hdc_value2/analyze2.py 同口径）。

### 2.3 防泄漏

评估用 **LOGO**（leave-one-goal-out）：测哪个 goal，训练集就剔除哪个
goal 的全部日志。否则"预测准"可能只是背答案。

## 3. 在线推理（搜索时）

```bash
./build/bin/gar_reason --mode agent --executor bnb \
    --bnb-priority hdc-value --value-model <meta.txt> \
    --csr <csr> --rules <rules> --goal 'rel(s,d)' --budget 2 ...
```

机制两处：

1. **evaluate 时**：候选分支优先级 = `cost + 预测剩余成本`
   （与原 `cost + min price` 同语义位置，仅替换预估项）。
2. **fork 时子分支继承父分支的模型 key**：deferred-evaluation 下
   未评估分支 key=0 按 FIFO 出队，会吞掉模型排序；继承父 key 让
   "有前途的父分支的孩子"先被评估——在线 A/B 证明这是机制对齐的
   必要一环（只改 evaluate 时几乎无效）。

**默认关闭**：不传 `--bnb-priority` 时全部行为与旧版逐字节一致。
开销：15~21µs/次推理（一次 4096 维点积），每 run 总计毫秒级，可忽略。

## 4. 实测效果

fb15k-237（rules=1652, DG arcs=3023, budget=2, cap=64, hybrid/exact, top_k=0, LOGO 模型）：

| goal | 展开数 util → hdc | wall util → hdc | 正确性 |
|---|---|---|---|
| athletes | 2084 → 2058（−1.2%） | 200.1s → 191.5s | best_cost/certified/chain 全同 |
| r2c_2 | 18962 → 1326（**−93%**） | 1781.2s → 124.9s | 同上 |
| r2c_4 | 12791 → 897（**−93%**） | 616.3s → 47.9s | 同上 |
| r2c_8 | 68 → 47（−30.9%） | 3.70s → 2.71s | 同上 |
| 合计 | 33905 → 4328（**−87.2%**） | 2711s → 367s | soundness pass |

synth 小图：机制与正确性全部正常，但 8~18 次展开的规模内**无区分度**
（收益 = 避免无效展开，小图没有无效展开可避免）。
出处：`experiments/hdc_online/`（summary.md / synth_summary.md）。

## 5. 边界与诚实注记

- **收益与搜索冗余度正相关**：无效分支越多越值钱；小图/平凡 goal 无收益。
- **goal 族覆盖决定有效性**：athletes 持平是因为其特征族（大 ncand root
  分支）在训练集里样本稀缺，模型预测近常数退化回 cost 序——是样本覆盖
  问题，不是机制问题。对策：按 goal 族补样本、多 seed 集成。
- **不保证必须是 HDC**：这里的编码实为"分箱状态特征的随机核展开"，
  未与多项式核/显式交互项/GBDT 做区分；论文表述应为"随机草图特征 +
  ridge 的学习型价值函数"，HDC 是其实现与工程选择（定长、位运算、
  零 ML 依赖、可增量编码）。
- 更大 budget 的可扩展性尚未验证（budget-3 树爆炸是当前瓶颈）。

## 6. 后续方向

按 goal 族补训练样本；多 seed 集成消方差；在线大 budget 扩展性；
表示层淘汰赛（HDC vs one-hot vs 多项式核 vs GBDT）以确定是否必须 HDC。

## 7. 三臂复核与资源边界（2026-11-06）

`experiments/hdc_hard_goals/ABLATION6_PREREGISTRATION.md` 的当前结果进一步说明，
必须把“价值学习有效”与“HDC 表示最优”分开：athletes/r2c_2/r2c_4 上 numeric
均优于 HDC，只有 r2c_8 上 HDC 以 47 展开、2.459s 优于 numeric 的 120 展开、
4.426s。四个锚点合计展开 HDC 4328、numeric 2437，numeric 少 43.7%。

HDC 对 utility 的精确 wall 加速为 athletes 1.086x、r2c_8 1.199x；r2c_2 的
utility 在 1002s 超时，HDC 109.383s 完成，只能声称 >9.142x 的保守下界。
r2c_4/utility 因整机内存截断，不能给当前精确加速比。所有完成的 10 个 run
均 proof_cost=2、certified_optimal=true，已写出派生 soundness 全部通过。

两个未入训困难 goal 因系统内存长期高于安全门槛，尚有 5 个 run 未完成，故困难
泛化仍为未验证。运行产物 `.work_hdcv/hdc_ablation6/` 保留全部 timeout/OOM，
不通过挑选获胜 goal 或替换容易 goal 来扩大 HDC 结论。

## 8. Fixpoint action-level Q（已完成 rules640 对照）

`q-hdc` / `q-numeric` 由 `LearnedQPolicy` 实现，`FixpointSession::ActionFeatures`
提供 11 个 action 特征，预测从当前 action（含）到 goal 的剩余实际求值次数。
两种模型只改变 ready action 顺序，不改变精确求值、派生、DG 过滤或 W 复活；
priority = −预测 cost-to-go。

训练通过 `--fixpoint-q-trace PATH` 获取，只有 goal 命中的 trace 具有限标签；
未命中 trace 与未观察 alternative 不分配人工标签。当前版本只收集
deterministic/random continuation，**没有实现 alternative-action rollouts**。
模型导出复用 `hdc_value_v1` / `numeric_ridge_v1`，但特征是 action 级，不可与
BNB 分支模型混用。

实验协议与故障修复记录见
`experiments/fixpoint_policy/FIXPOINT_Q640_PREREGISTRATION.md`。在 rules640、
12 个隔离训练 goal、36 个测试 goal 上，两种模型共同使用 5,757 行训练数据。
修复 ready 提前终止后，D、numeric-Q、原始 HDC-Q 均为 36/36 命中，soundness=1，
无 timeout，每臂有 9 次 cap。原始 HDC-Q 比 numeric 多 4.266% 求值且更慢，
不支持 HDC 排序收益。原始结果见 `.work_hdcv/fixpoint_q640/summary.md`。

### 8.1 bit / SIMD value-model kernel

`ValueModel` 现支持 `original`、`simd`、`bit-lut` 三种 HDC 后端，由
`--hdc-value-backend` 选择，默认 `bit-lut`。SIMD 用 AVX2 完成在线 int16
bundle/bind 和加权点积。bit-LUT 利用分箱特征的有限词表，在加载期以 bit-packed
XOR 和 AVX-512 预计算 `w·v` 及 `w·(v_a⊙v_b)`；在线预测只做 16 项左右的查表
与加法。它不改变 HDC 维度、训练行、ridge 权重或 action 语义。

在同一 q_hdc 模型、同一 36 goal 的 108-run 独立批次中：

| 后端 | 宏平均外部 wall(s) | 宏平均内部 wall(s) | 宏平均模型推理(ms) |
|---|---:|---:|---:|
| original | 9.166 | 8.102 | 1489.165 |
| SIMD | 8.530 | 7.462 | 755.987 |
| bit-LUT | 7.684 | 6.620 | 14.518 |

bit-LUT 相对同批次 original 的外部/内部 wall 加速为 1.193x/1.224x，模型推理
加速为 102.576x。108/108 run 完成，soundness=1，36 个 goal 上三种后端的
chain 均与原始 HDC-Q 逐字节一致。优化消除了绝大部分模型计算开销，但求值次数
不变，**不是排序质量或搜索空间的改善**。结果见
`.work_hdcv/fixpoint_q640_kernels/summary.md`。
