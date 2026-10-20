# 需求文档（最终版留档）

GARAgent：面向论文《Agentized Graph Reasoning via Policy-Guided Deduction》的
GAR（图关联规则）算法库。本文档是需求演进的最终合并版（v0.1→v0.4），只收录
已有实证的特性；性能数字均为本仓回归实测。

## 演进沿革

- **v0.1 规则内核**：CSR（RAPIDS/cuGraph 布局）+ YAML 的规则表示，标量位宽与
  MatrixGraph 对齐（uint32）；规则加载/序列化/校验。
- **v0.2 挖掘与闭包演绎**：AMIE 式链式挖掘（ChainMiner）与通用拓扑挖掘
  （PatternMiner）；CPU chase（oneTBB 轮内规则级并行）与 GPU chase
  （MatrixGraph task::GARDeduce），双端同语义同输出布局。
- **v0.3 策略制导演绎（agent）**：论文 §3-§5 的 agent 模型——motif、见证、
  失败注册、打分、策略、执行器；greedy 与 HybNS 分支定界双执行器；
  HDC 数值内核支撑证据打分。
- **v0.4 分析工具链**：谓词表/依赖图（SCC）/规则统计/目标采样；
  gar_analyze 五子命令；verify 三方对账；workload 构造（hold-out + B* 标定）。

分层重构（六阶段，见 git log）把仓库定型为五级模块结构（docs/agent_design.md）。

## 变体矩阵

| 变体 | 入口 | 引擎 | 实证 |
|---|---|---|---|
| full-cpu | `gar_reason --mode full --engine cpu`（薄壳 `deduce`） | CpuChaseEngine（oneTBB，`--threads` 限并行度） | fb15k 全闭包 10M 上限 4 轮 17.6s；fb 2 轮 648,595 事实逐字节对账 |
| full-gpu | `gar_reason --mode full --engine gpu`（薄壳 `deduce_gpu`） | GpuDeduceEngine（GARDeduce） | 同上，与 CPU 逐字节一致；全闭包 49s |
| agent | `gar_reason --mode agent` | ReasoningAgent + Greedy/BranchBound | synth 2 步最优链（certified）；fb15k round-2 目标 2 步链（greedy 400 步不得，bnb 带证书达成） |

## 功能需求终态

### F1 规则表示与挖掘（既有，未变）
一规则一目录（rule.yaml + csr_*.bin）；链式 + 通用拓扑挖掘；规则检验
validate_rules（PASS/DRIFT/SKIP，fb15k 1,652/1,652 PASS）。

### F2 全闭包演绎（full 模式）
朴素 chase 至不动点（cap 截断标记）；CPU/GPU 双引擎同口径（homomorphism
匹配 + 顶点 label 后过滤 + X 逐 embedding 求值 + 轮末批量并图）；输出
summary.yaml / closure_edges.tsv / matches/ 三件套，双端逐字节一致。

### F3 策略族（agent 模式）
deterministic（argmax，平局取下标小者）/ random（softmax(total/τ)，固定种子
可复现）/ hybrid（admissible 硬过滤 + argmax，Corollary 4 的完备性保持）。

### F4 双打分器
- ExactScorer：rel_evd = 见证前提数/前提总数（链级 exact 见证集）。
- HdcScorer：rel_evd = cos(v_μ, E)，v_μ = 前提事实编码的 bundle，E = 链级
  HDC 证据向量；dcost 的命中判定阈值 τ=0.15（App I 噪声尺度 1/√|E| 的定值化）。
- 公共式：rel_fam = γ^{Σ去重谓词 d(q)}（ScoreF 口径，任一不可达 → 0）；
  total = α·(rel_fam + ν·rel_evd + ν2·conf) − β·dcost；price = w₀ + d(p₀)。

### F5 执行器
- GreedyExecutor：§5.1 提交式单链；成败均计费；终止 = goal/no_candidates/budget。
- BranchBoundExecutor（Figure 6 HybNS）：best-first 分支定界，延迟求值优先级
  （cost + min price），top-k=0 精确模式带 Theorem 5 最优性证书 /
  certified_no_chain 预算不足证书 + anytime 贪心回退；top-k>0 截断无证书。
  规模注意：精确 bnb 在大 DG 邻域展开数随等价 price 候选数组合增长
  （fb15k 实测 ~0.1s + ~40MB/分支），用 --top-k/--max-candidates-per-rule/
  紧凑 budget 控制。

### F6 依赖图与分析
谓词身份规则（关系名含 ^-1 归一化 / 属性名 / 模型名）；BuildDG 全图一次构建、
d(·) 按 goal 反向 BFS（w0 加权 Dijkstra）；Tarjan SCC（递归族识别）；
dot/yaml 序列化；前向导出深度 DerivationDepths（B* 下界口径）。

### F7 分析工具链（gar_analyze）
rules-stats / gen-goals（hold-out + chase 可达校验 + B* 双口径）/
gen-workload（预算梯度 {0.5,1,2,4}×max(bstar_dg,1)）/ chain-cmp /
verify（CONSISTENT / BUDGET-LIMITED / DIVERGENT / INFEASIBLE 四值判定；
--run 现场三引擎对账、--from-dirs 离线对账、--tasks 批量）。

### F8 verify 三方对账
agent 导出 ⊆ cpu 闭包（soundness）；goal 三方判定一致；
预算语义判定（budget < bstar_dg 或 budget_exhausted → BUDGET-LIMITED 属预期）。
批量模式汇总成功率（达成/可行）。实证：synth 12 任务全 CONSISTENT。

## HDC 决策（最终版）

- bit-packed 双极超向量（1=+1/0=−1，XOR=binding）；维度运行时可调，
  档位 = 2 的幂 {128…16384}，**默认 8192**（√8192≈90.5 覆盖 m+N≤90 的
  App I 可靠性条件，1KB/向量；ChooseDim 按 (m+N)²≤D 选档，超档封顶 16384）。
- 事实编码 v_f = v_p ⊕ ⨁ᵢ ρⁱ(v_{aᵢ})；bundle = 逐位多数表决
  （k≤3 位技巧，k≥4 int16 计数）；**tie-break 由 bundle 内容派生**
  （参数 id 哈希合成 tie 向量）——全局共享 tie 向量会向所有 bundle 泄漏
  ≈0.25 余弦相关性（实测发现并修复）；与论文随机 tie-break 期望一致、
  运行间可复现。
- 符号在线哈希（seed⊕id → splitmix64），零缓存零实体化；Fact→SymbolId
  绑定在 engine/agent/witness.h（谓词名 FNV-1a→splitmix64 置 bit63，
  顶点符号 = 裸 vertex id）。
- 实证（/tmp 临时验证程序，已删）：D=16384、m=n=50 时 cos ≈ c/√(m·n)
  最大误差 0.0033（容差 2/√D=0.0156）。

## MatrixGraph 边界

全仓只有**两个 sanctioned 接触点**：
1. `bridge/`（ImmutableCSR/属性列加载、规则→GARPatternArrays 转换）；
2. `engine/deduction` 的 GpuDeduceEngine 实现文件（调用 task::GARDeduce 的
   host API，纯 .cpp——header 只含 __host__ 声明，kernel 在 matrixgraph_core；
   仅因 DeviceOwnedBuffer 内联 cudaMalloc/cudaFree 链 CUDA::cudart）。
其余模块一律不见 MatrixGraph 类型（chase.h 的 "the engine never sees
MatrixGraph types" 语义从 phase 1 保持至今）。
