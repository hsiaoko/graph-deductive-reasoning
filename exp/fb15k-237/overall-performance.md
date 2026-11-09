# FB15K-237：Fixpoint Overall Performance

## 1. 实验目的

在同一图、同一规则集合和同一批测试 goal 上，比较：

1. GoalChase：只保留 goal-relevant 规则，ready action 按固定顺序执行，不使用证据或成本排序；
2. Deterministic（原 Fixpoint-D）：选择精确组合分数最高的 ready action；
3. Random（原 Fixpoint-R）：按精确分数的 softmax 分布采样，使用 seeds `{7,13,42}`；
4. Learned numeric-Q：数值 ridge 模型预测 action 的剩余求值成本；
5. Learned HDC-Q：HDC 特征表示加 ridge 模型预测同一成本。

这些变体只改变 ready action 的执行顺序。规则过滤、图匹配、前提求值、事实派生、挂起与复活均使用相同的精确符号实现。

## 2. 数据与规则

- 图：`/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr`
- 规则：`.work_hdcv/fixpoint_rules640/rules640`，共 640 条
- 完整 Chase ground truth：`.work_hdcv/fixpoint_rules640/runs/chase/rep1`
- Chase 结果：28 轮到达 fixpoint，派生 5,019,342 条事实
- Chase 三次 cold wall：742.462s、835.628s、690.885s；中位数 742.462s
- Chase 全轮完整匹配：13,041,165,813

Chase 计算完整闭包，Fixpoint 在指定 goal 派生后立即停止。二者的 wall 对照反映“36 个目标查询相对于一次完整闭包”的成本，不是相同输出规模下的等价工作量比较。

## 3. 训练与测试隔离

测试集完整保留 `.work_hdcv/fixpoint_rules640/goals.tsv` 的 36 个 goal。

训练 goal 从 rules640 完整闭包中排除这 36 个测试 goal 后冻结：

- round 1、round 2、round >= 3 各 4 个，共 12 个；
- 选择盐：`20261106-fixpoint-q-train-v1`；
- 训练 goal：`.work_hdcv/fixpoint_q640/train_goals.tsv`；
- 测试 goal 副本：`.work_hdcv/fixpoint_q640/test_goals.tsv`；
- 已验证训练与测试 goal spec 不重叠。

每个训练 goal 分别运行 deterministic 和 random(seed=7) continuation，共 24 个 trace run。23 个命中 goal；`t11/deterministic` 在内部 900 秒上限终止，不用于回归，也不赋予人工有限成本标签。

只有成功 trace 具有限标签。对第 `i` 个实际求值 action，若轨迹总计求值 `N` 次，则：

```text
remaining_evaluations = N - i + 1
```

该标签包含当前 action。未观察到的 alternative action 不伪造标签；本实验没有执行 alternative-action rollouts。每条成功 trace 按剩余成本排序后等距最多采样 256 行，两种模型共同使用 5,757 行训练数据。

## 4. 模型与 action 特征

两种模型使用相同的 11 个原始特征，顺序固定为：

```text
price, pattern_edges, log_support, relfam, relevd, conf,
dcost, total, dp0, wit, premises
```

共同交互项为：

```text
(price,relevd), (log_support,dcost), (conf,total), (dp0,wit)
```

- numeric-Q：标准化原始特征 + 相同交互项 + ridge；
- HDC-Q：4096 维分箱双极随机特征 + 相同交互项的绑定 + ridge；
- 分箱参数：`bin_cap=7`、`bin_scale=2.0`；两种 ridge 均使用 `lambda=1.0`；
- 模型输出：从当前 action（含）到 goal 的预计剩余求值次数；
- 排序键：`priority = -predicted_remaining_evaluations`，即优先选择预测剩余成本较低的 action。

## 5. 正式对照参数

当前主实验采用 `experiments/fixpoint_policy/UNIFIED640_PREREGISTRATION.md` 的统一协议。五种方法均执行 36 goals × seeds `{7,13,42}`，每臂 108 run，共 540 run。所有方法使用同一图、640 条规则和同一测试 goal 集；两个 Learned 臂分别使用冻结的已有模型文件。逐 goal 取三 run 中位数后再做 36-goal 宏平均。统一输出为 `.work_hdcv/fixpoint_unified640/`。

| 共同参数 | 五种方法的统一值 |
|---|---:|
| executor / star order | fixpoint / goal |
| fair interval | 0 |
| policy seeds | 7, 13, 42 |
| budget | 1,000,000,000 |
| max candidates/rule | 100,000 |
| print | 0 |
| 外部/内部时间上限 | 1000s / 900s |
| 进程树/整机已用内存上限 | 256 GiB / 256 GiB |

| 方法 | policy | scorer | model / 实现差异 |
|---|---|---|---|
| GoalChase | deterministic | null | 不使用证据或成本排序 |
| Deterministic | deterministic | exact | 选择组合分数最高的 action |
| Random | random | exact | Gumbel-max，与 softmax(total/tau) 采样分布等价 |
| Learned numeric-Q | q-numeric | exact | `q_numeric.meta` |
| Learned HDC-Q | q-hdc | exact | `q_hdc.meta`，原始 HDC 后端 |

除上述策略及其实现所需配置外，其余参数完全一致。GoalChase 的 NullScorer 只保留符号可达性与结论去重，所有 ready action 的选择键均为 0，按固定候选顺序破同分。五臂共享 DG goal-relevance 过滤、候选生成、精确前提求值、事实派生、挂起/复活和 goal 提前停止。非随机方法不消费 policy 随机数，三个 seed 是相同逻辑的计时重复。

## 6. 总体结果

540/540 run 均正常结束并命中 goal，soundness=100%，无 timeout；每臂有 27 次 candidate cap。Fixpoint 没有 BNB 树节点，因此用 `fx_select_events` 表示实际选择/展开的分支数；它包括后来被零成本跳过的已知结论。`fx_evaluate_events` 另列为真实前提求值次数，`fx_ready_insertions` 则是生成的 ready 候选数，三者不能混用。外部 wall 包含加载与写盘，内部 wall 只包围 `ReasoningAgent::Run`。

| 方法 | goal 命中 | soundness | 宏平均外部 wall(s) | 宏平均内部 wall(s) | 宏平均选择分支 | 宏平均 ready 候选 | 宏平均完整匹配 | 宏平均求值/派生 | 36-goal 代表外部 wall 合计(s) | cap | timeout |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| GoalChase | 108/108 | 100% | 7.892 | 6.777 | 111,500.6 | 144,573.1 | 479,257.3 | 47,341.7 | 284.114 | 27 | 0 |
| Deterministic | 108/108 | 100% | 7.554 | 6.482 | 104,896.8 | 125,822.1 | 455,325.2 | 45,616.1 | 271.945 | 27 | 0 |
| Random | 108/108 | 100% | 8.036 | 6.915 | 103,110.9 | 132,445.3 | 472,083.4 | 46,027.8 | 289.291 | 27 | 0 |
| Learned numeric-Q | 108/108 | 100% | 7.375 | 6.286 | 97,894.9 | 120,290.3 | 452,180.9 | 44,214.6 | 265.486 | 27 | 0 |
| Learned HDC-Q（原始后端） | 108/108 | 100% | 9.192 | 8.105 | 98,394.2 | 120,039.6 | 454,652.6 | 45,745.1 | 330.895 | 27 | 0 |

统一设置下，相对 Deterministic：

| 方法 | 外部 wall 加速 | 内部 wall 加速 | 选择分支减少 | 真实求值减少 |
|---|---:|---:|---:|---:|
| GoalChase | 0.957x | 0.956x | -6.296% | -3.783% |
| Random | 0.940x | 0.937x | 1.703% | -0.903% |
| Learned numeric-Q | 1.024x | 1.031x | 6.675% | 3.072% |
| Learned HDC-Q（原始后端） | 0.822x | 0.800x | 6.199% | -0.283% |

numeric-Q 在该统一设置中取得最低平均时间和真实求值数，但时间收益仅约 2–3%。Random 的选择分支略少，真实求值却略多，整体慢于 Deterministic。HDC-Q 减少了选择分支，但真实求值没有减少，原始高维模型开销使其最慢；不能据此宣称 HDC 排序优于 numeric-Q。

作为非等价工作量的参照，GoalChase、Deterministic、Random、numeric-Q、HDC-Q 的 36-goal 代表外部 wall 合计与一次完整 Chase 中位数 742.462s 之比分别为 0.383、0.366、0.390、0.358、0.446。Chase 计算完整闭包，五种方法均在指定 goal 派生后停止。

## 7. HDC-Q 为什么慢

本节保留旧 Q 批次（每 goal 单次、Q 臂 fair=1024）的细粒度开销归因，用于解释模型成本；这些数字不是第 6 节统一矩阵的聚合结果，五方法比较以第 6 节为准。

结论是：**模型计算本身很慢，同时学习排序没有减少搜索；两者共同造成当前结果。**

### 7.1 直接的模型计算开销

| 指标 | numeric-Q | HDC-Q |
|---|---:|---:|
| 36-goal 模型调用总数 | 4,649,423 | 4,599,898 |
| 宏平均模型推理时间 | 0.008485s | 1.558211s |
| 按总调用数加权的单次推理时间 | 0.0657 us | 12.195 us |

HDC 单次模型计算约为 numeric 的 186 倍。当前 HDC 推理每次需要构造并清零 4096 维 `int16` 向量，执行 11 个特征的向量累加、4 个逐坐标绑定、偏置向量累加和 4096 维点积；numeric 只需十几个标准化数值特征、4 个乘积和一次短点积。实现位置为 `engine/agent/src/value_model.cpp`。

按宏平均账面值计算：

- HDC-Q 相对 numeric-Q 多耗时 `9.540 - 7.774 = 1.766s/goal`；
- 两者模型推理时间差为 `1.558 - 0.008 = 1.550s/goal`，约为额外 wall 的 87.7%；
- HDC-Q 相对 D 多耗时 1.912s/goal，其模型推理时间约为该差值的 81.5%。

这些计时包含在生成、派生和 delta 等阶段中，不能把各阶段时间再简单相加；上述比例是账面归因，不是同序 replay 的严格因果消融。

### 7.2 排序没有带来可抵消开销的搜索收益

如果 HDC 排序有效，应当以减少 action 求值、完整匹配或后续 delta 工作来抵消模型成本。实际结果是：

- 相对 D，HDC-Q 求值次数增加 0.123%，完整匹配只减少 0.121%；
- 相对 numeric-Q，HDC-Q 求值次数增加 4.266%，完整匹配增加 0.677%；
- 配对求值减少的中位数为 0，wall 只在 7/36 个 goal 上优于 numeric-Q。

因此，HDC-Q 没有在这组 goal 上学到稳定优于现有精确特征排序的成本估计。它不是“搜索量大幅减少、只是向量计算略贵”，而是“搜索量没有改善，同时每次排序都支付高维模型计算成本”。

该结论仅适用于当前 rules640、训练轨迹、11 个特征和 4096 维表示，不能外推为 HDC 在所有图推理场景都无效。

## 8. 正确性与故障记录

统一矩阵的 540/540 run 均正常结束并命中 goal，所有导出派生均属于完整 Chase 闭包。每臂有 27 次 candidate cap，集中于 `p11`、`p21`、`p32`、`p34`；因此不宣称这些设置具有无截断的全局完备性。五臂均无 timeout，最大单 run 进程树 RSS 为 5.554 GiB。GoalChase、Deterministic 和两个 Q 臂在所有 36 个 goal 上均有跨三个 seed 逐字节一致的 chain，符合其非随机执行语义。

训练阶段的 7,309,499 条派生也全部通过 Chase 闭包检查。C++ 与 Python 的模型预测已交叉验证，误差仅为浮点累加量级；最终默认 deterministic 的 `p25` chain 与历史结果逐字节一致。

首轮旧 Q 实验曾发生提前 `no_candidates`：结论已存在的 action 被 admissible 过滤从优先级堆丢弃，却仍滞留在 ready 集合，阻止后续 star 展开。修复后，这类 action 统一走原有的零成本 `skip-known` 路径。错误结果保存在 `.work_hdcv/fixpoint_q640/runs_admissible_bug/` 和 `summary_admissible_bug.*`，仅用于审计，不用于性能结论。

## 9. 产物与复现命令

- 统一协议：`experiments/fixpoint_policy/UNIFIED640_PREREGISTRATION.md`
- 统一运行与汇总脚本：`experiments/fixpoint_policy/run_unified640.py`
- 五方法主汇总：`.work_hdcv/fixpoint_unified640/summary.md`、`summary.json`
- 五方法逐 run 与逐 goal 数据：`.work_hdcv/fixpoint_unified640/rows.tsv`、`per_goal.tsv`
- 共同模型：`.work_hdcv/fixpoint_q640/models/`
- 历史实验目录保留在 `.work_hdcv/fixpoint_rules640/`、`.work_hdcv/fixpoint_q640/` 和 `.work_hdcv/fixpoint_goal_chase640/`，不与主表混合计算。

```bash
cmake --build build -j 8
python3 experiments/fixpoint_policy/run_unified640.py --phase all
```

已有 `resource.json` 的 run 会被跳过，不会静默覆盖历史测量；`--phase aggregate` 可单独重建统一汇总。


## 10. 后续优化：bit / SIMD HDC value-model kernel

第 6 节统一主表固定原始稠密 HDC 后端。本节是独立的旧 Q 批次内核消融（每 goal 单次、fair=1024），在同一 q_hdc 模型和同一 36 个 goal 上比较原始实现、在线 AVX2 SIMD、bit-packed 加载期 LUT 预计算；不与统一五方法主表混合计算。该实验不重新训练模型，也不改变任何逻辑求值。

bit-LUT 利用 ridge 点积对 bundle/bind 的线性分解，把有限分箱词表中的 `w·v` 和 `w·(v_a⊙v_b)` 移到模型加载期。加载时使用 bit-packed XOR 与 AVX-512；在线预测仅做少量表查找和加法。

| HDC 后端 | 宏平均外部 wall(s) | 宏平均内部 wall(s) | 宏平均选择分支 | 宏平均 ready 候选 | 宏平均求值 | 宏平均模型推理(ms) | 加权 us/call |
|---|---:|---:|---:|---:|---:|---:|---:|
| original | 9.166 | 8.102 | 98,074.3 | 119,946.5 | 45,672.3 | 1,489.165 | 11.6546 |
| SIMD | 8.530 | 7.462 | 98,074.3 | 119,946.5 | 45,672.3 | 755.987 | 5.9166 |
| bit-LUT | 7.684 | 6.620 | 98,074.3 | 119,946.5 | 45,672.3 | 14.518 | 0.1136 |

相对同批次 original，SIMD 的外部/内部 wall 加速为 1.075x/1.086x；bit-LUT 为 1.193x/1.224x，模型推理加速为 102.576x。独立微基准（500,000 calls）的 wall/call 分别为 11.299us、5.845us、0.121us；bit-LUT 加载期预计算约 3.3ms。

108/108 run 正常完成，soundness=1，无 timeout，每后端有 9 次 cap；三种后端在所有 36 个 goal 上的 chain 均与原始 HDC-Q 逐字节一致。这证明当前优化是计算内核加速，不改变学习排序。

bit-LUT 的总 wall 为 276.637s；此前 D/numeric-Q 的不同批次参照为 274.627s/279.870s。它基本消除了 HDC 的额外模型成本，但宏平均求值不变，不能将其写成搜索策略本身的改进。跨批次的小幅 D/numeric 比较只作参照，不作为确定性优胜结论。

产物与命令：

```bash
python3 experiments/fixpoint_policy/run_hdc_kernels.py --phase eval
python3 experiments/fixpoint_policy/aggregate_hdc_kernels.py
./build/bin/value_model_bench \
  .work_hdcv/fixpoint_q640/models/q_hdc.meta bit-lut 500000
```

结果位于 `.work_hdcv/fixpoint_q640_kernels/summary.md`、`summary.json` 和 `rows.tsv`。
