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

- **11 个状态特征**（全部来自既有精确模块）：
  `cost, clen, ncand, minprice, relfam, relevd, conf, dcost, total, dp0, wit`。
- **分箱**：连续值离散化（cap=7 档，scale=2.0）。
- **随机向量编码**：每个 (特征, 档位) 分配一个 D=4096 维随机双极向量
  （int8）；再构造 4 组**合取特征**（cost×dp0、ncand×dcost、conf×total、
  cost×clen），使模型能表达"距离近但见证少"这类组合规律——这是它优于
  纯线性工程特征的关键。
- **bundle 成分支指纹 → ridge（λ=1.0）回归剩余成本**。
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

fb15k-237（rules=3023, budget=2, cap=64, hybrid/exact, top_k=0, LOGO 模型）：

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
