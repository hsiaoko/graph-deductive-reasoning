# 推理整体流程（输入 GAR 集合 + 图 → 派生）

本文按代码真实执行顺序介绍推理主流程；架构分层见 `agent_design.md`，
命令细节见 `commands.md`，实验数字出处见 `experiments/` 各目录。

## 0. 启动时的准备（一次性）

**装载**：

- 图：MatrixGraph 的 ImmutableCSR，经 `bridge` 转成内部的 TripleGraph
  （三元组 + 邻接索引）。
- 规则：每条 GAR = 一个目录（`rules.yaml` 谓词 + `csr` 拓扑），由
  RuleLoader 读入。

**建依赖图 DependencyGraph（DG）**：这是后面打分的原料。把每条规则看成
"前提谓词 → 结论谓词"的依赖弧，全体规则合成一张谓词级有向图。给定 goal
后，从 goal 谓词反向最短路，得到每个谓词离 goal 还差几层 d(p)；同时用
Tarjan 算法找出递归族（互相依赖成环的谓词团）。

## 1. 两种总模式

```bash
gar_reason --mode full    # 全量 chase：不要 goal，所有规则轮轮全试，算完整闭包
gar_reason --mode agent   # 目标制导：只针对一个 goal 推理（论文主线）
```

agent 模式的主类是 **ReasoningAgent**，由五个可插拔部件组成：候选生成器
MotifGenerator、Scorer、Policy、Executor、成本模型。scoring 和 policy 就在
下面的主循环里。

## 2. Agent 主循环（每一轮做什么）

```
while goal 未推出 且 budget 未用完:
    ① 生成候选：哪些规则、哪些匹配，此刻有希望通过？
    ② Scorer 给每个候选打分
    ③ Policy 按分数决定先用谁
    ④ 应用：精确校验前提 → 派生新事实 → 更新状态
    ⑤ 记账：见证日志 + 失败注册 + 预算扣减
```

**② Scorer 的打分公式**（每个候选 motif μ）：

```
rel_fam = γ^(各前提谓词到 goal 的距离之和)     ← 离 goal 近不近（DG 提供）
rel_evd = 已见证前提数 / 总前提数              ← 前提满足多少（精确哈希查询）
conf    = 规则挖掘时的置信度                    ← 规则本身靠不靠谱
price   = 1 + d(p₀)                            ← 这一步的预估代价
total   = α·(rel_fam + ν·rel_evd + ν₂·conf) − β·dcost
```

**③ Policy 决定"怎么用分数"**：

- `deterministic`：永远选 total 最高的（默认）
- `random`：按分数抽样（对照组）
- `hybrid`：混合
- `jev`：问 LLM（预留，未启用）

**④ "应用一步"和 chase 是同一个动作**：同一个 `EvalAntecedent` 校验 +
同态匹配，所以 agent 推出的每条事实都合法——soundness 的构造性保证。

**⑤ 记账**：成功就把派生事实和前提记入见证集；失败的候选按类型注册
（只读图上的失败永久退休，可派生谓词的失败等图更新后再试），避免重复
烧预算。

## 3. Executor 决定"搜索的走法"（三种，可替换）

- **greedy**：沿单条路径走，每轮选分最高的，不回头。快但会被 decoy 带偏。
- **bnb**：保留整棵分支树 + 界限剪枝，能证明最优链。慢、吃内存。
- **backward（主引擎）**：不建前向树，从 goal 反向分解。

## 4. Backward 的实际算法流程（主引擎）

以 `goal = award(A,B)` 为例：

```
1. 用 head 谓词索引，只取"结论 = award"的规则
2. 把 A、B 钉进规则的结论槽位（pinning），做带钉匹配
3. 对每条候选规则：
   ├─ 前提全能在图里匹配上 → 直接派生 goal，结束
   ├─ 缺一条前提边 → 把这条边变成新子 goal，递归（深度≤4，预算逐层减半）
   └─ 缺两条前提边（双侧）→
        a. 两侧各收集候选值（现成边 ∪ 可派生值，只登记不花钱）
        b. 用剩余前提边做半连接求交，筛出能同时满足全部约束的组合
        c. 只对存活组合付费派生，再全模式校验 → 派生 goal
4. 全程共享 budget，每次真实派生计 1；推出 goal 即停
```

实测（fb15k-237 + rules_g3 81,683 规则，20 个随机可达任务，budget=10）：
20/20 命中，每目标 70ms~2.2s，派生 1~6 条事实；对照全量 chase
227s + 48GB + 2350 万条附带派生（2 轮截断口径）。
出处：`experiments/fb15k_g3_hitfix/`。

## 5. 输出与对账

每次运行产出：

- `chain.tsv`：推导链（每步用了哪条规则、派生了什么）
- `closure_edges.tsv`：全部派生事实
- `summary.yaml`：步数 / 耗时 / 命中 / 终止原因

`gar_analyze verify` 拿结果和全量 chase 闭包对账：agent 派生 ⊆ chase
闭包、hold-out 下 goal 一致，判 CONSISTENT / BUDGET-LIMITED / DIVERGENT。

## 6. HDC 在流程里的位置

- **rel_evd 的 HDC 版**：仍是可选 scorer（`--scorer hdc`），实验证明
  相对 exact 无命中率收益且更慢（`experiments/hdc_fair/`），默认用 exact。
- **HDC 价值函数给 bnb 排序（已接入在线主循环）**：
  用 HDC 随机特征 + ridge 学习"分支状态 → 剩余成本"，作为 bnb
  best-first 的可选优先级。**只改展开顺序，不动 bound、剪枝与派生校验**，
  最优性证书语义不变。默认关闭，开启方式：

  ```bash
  gar_reason --mode agent --executor bnb \
      --bnb-priority hdc-value --value-model <模型文件> ...
  ```

  在线 A/B（budget=2, cap=64, LOGO 模型防泄漏，4 个非平凡 goal）：
  展开数合计 33905 → 4328（**−87.2%**），wall 2711s → 367s（−86.4%）；
  两个硬 goal 各 −93%；最优成本 / certified / chain.tsv 全部一致，
  soundness pass；模型推理 15~21µs/次，可忽略。
  出处：`experiments/hdc_online/`（commit `1f5a0f6` / `66fb175`）。
