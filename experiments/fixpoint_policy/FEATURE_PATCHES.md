# Hybrid + Binary 标签：代码改动清单（供审阅/提交）

2026-11-21。两个 feature 的代码改动目前都在工作树，与作者在这些文件上的在途重构交织
（尤其 `tools/cli/agent_mode.h`，348 行改动大部分是作者的 learned-policy CLI 接线）。
本文精确列出*本诊断工作*新增的内容，便于作者审阅后随核心重构一并提交，或据此手动拆分。
助手未对这些核心文件做 git 提交（避免误伤作者在途工作）；已提交的只有 `experiments/` 下的
脚本与诊断文档。

## Feature 1：backward-first hybrid executor（`--search-direction hybrid`）

实测增益：derived −18.9%、execution −14.2%、hit 持平（见 LEARNED_POLICY_DIAGNOSIS.md）。

- `core/engine/agent/include/gar/core/agent/config.h`
  - `enum class SearchDirection` 增加 `kHybrid`（带「backward 优先 + 回退 forward」注释）。
  - `SearchDirectionName` 由三目改为 switch，覆盖 forward/backward/hybrid。
  - 注意：同文件 `PolicyKind` 的 `kUniformRandom` 增、`kQHdc/kQNumeric` 删是*作者*改动，非本工作。
- `core/engine/agent/include/gar/core/agent/executor.h`
  - 新增 `class HybridExecutor final : public Executor` 声明（带机制注释）。
- `core/engine/agent/src/hybrid_executor.cpp`（**新文件，纯本工作**）
  - `HybridExecutor::Run`：先 `BackwardExecutor.Run`，`goal_derived` 即返回；否则
    `AdaDeduceExecutor.Run` 继续。合并 chain + execution_cost_ms + logical_cost；
    **不合并 derived**（两 executor 都 `derived = state->journal()`，forward 已含 backward 事实）。
- `core/engine/agent/CMakeLists.txt`
  - `gar_engine_agent` 源列表加 `src/hybrid_executor.cpp`（紧随 backward_executor.cpp）。
- `tools/cli/agent_mode.h`
  - `ParseSearchDirection`：加 `if (value == "hybrid") return SearchDirection::kHybrid;`，
    错误提示改 `(forward|backward|hybrid)`。
  - `MakeReasoningAgent` executor 选择：加 `else if (kHybrid) comps.executor =
    std::make_unique<agent::HybridExecutor>();`。

## Feature 2：binary 证明有用性标签（`gar_train_policy --label binary`）

结论：纯 learned 几乎追平 Det（derived 46287 vs graded 49409），但仍不赢 Det（RC3）。

- `core/engine/agent/include/gar/core/agent/learning/proof_dataset.h`
  - 新增 `BuildBinaryProofDataset` 声明 + 注释（on-proof=1/off-proof=0）。
- `core/engine/agent/src/learning/proof_dataset.cpp`
  - 抽出匿名命名空间 `FinalizeDataset(positives, negatives*, config, dataset*)`：冲突负样本
    剔除 + 正样本稳定排序 + 每 goal 哈希采样负样本（原 BuildRelevanceDataset 尾部）。
  - 原 `BuildRelevanceDataset` 主体改为文件内 `static BuildProofLabeledDataset(traces, config,
    bool binary_targets)`；正样本 target = `binary_targets ? 1.0 : pow(label_gamma, distance)`。
  - `BuildRelevanceDataset` / `BuildBinaryProofDataset` 为薄包装（binary_targets=false/true）。
- `tools/cli/gar_train_policy.cpp`（**整文件未跟踪，属作者 learning 模块**）
  - `Args` 加 `std::string label = "proof";`；解析 `--label`；校验 `proof|binary`；
    `build_dataset = (label=="binary") ? BuildBinaryProofDataset : BuildRelevanceDataset`；
    report.json 加 `"label"` 字段；usage 加 `[--label proof|binary]`。

## 本工作新增的实验脚本（已提交）

- `run_blend_ablation.py`：learned 权重消融（`--handcrafted/--relevance/--model/--tag`）。
- `run_relevance_scale.py`：从 chase 闭包采样可派生 goal 扩数据重训。
- `run_backward_headroom.py`：backward 搜索头room 量化。
- `run_hybrid_eval.py`：hybrid/forward/backward 端到端评测。
- `run_oracle_bound.py`：证明 DAG 大小（oracle 最小探索）vs Det derived。
- `run_exploration_probe.py`：硬 goal 探索 rollout vs Det。

## Feature 3：learned 策略选择器（路径 C，`--search-direction learned-strategy`）

实测赢 Det：derived −18.1% / execution −16.1% / hit 持平（37701 / 2.051s）。

- `core/engine/agent/include/gar/core/agent/config.h`：`SearchDirection::kLearnedStrategy` +
  SearchDirectionName case。
- `core/engine/agent/include/gar/core/agent/executor.h`：`ExecConfig::strategy_model_path` 字段 +
  `LearnedStrategyExecutor` 声明。
- `core/engine/agent/src/learned_strategy_executor.cpp`（新文件）：加载 gar_strategy_v1 logistic
  模型，算 goal cone_size（GoalConeRuleIndices）+ pred_degree（RelationPairs），sigmoid 概率
  ≥threshold 委托 HybridExecutor，否则 AdaDeduceExecutor forward。
- `core/engine/agent/CMakeLists.txt`：加 `src/learned_strategy_executor.cpp`。
- `tools/cli/agent_mode.h`：`--strategy-model` flag + ParseSearchDirection learned-strategy +
  MakeExecConfig.strategy_model_path + executor 选择器分支。
- 训练数据/工具：`run_backward_labels.py`（backward 成败标签）、`gar_goal_features.cpp`（cone+度
  特征，CMake target）、sklearn logistic（参数存 .work_hdcv/strategy/strategy.model）。

## 诊断探针代码（工作树，与作者 CMake 交织）

一次性诊断工具（非产品面），证伪路径 A 用，构建绿：

- `tools/cli/gar_hdc_probe.cpp`（新文件）：HDC 余弦 vs f 的证明成员身份 AUC，分层。
- `tools/cli/gar_graph_probe.cpp`（新文件）：多跳 BFS 图距离 vs f 的证明成员身份 AUC，分层。
- `tools/cli/gar_goal_features.cpp`（新文件）：goal cone_size + pred_degree 特征（C 训练用）。
- `tools/cli/CMakeLists.txt`：加 `gar_hdc_probe` / `gar_graph_probe` / `gar_goal_features` 三个
  target（镜像 gar_train_policy，纯 C++ 链接 graph/analysis/engine::agent）。该文件含作者在途
  改动，探针 target 为加性 hunk，留作者一并提交。

## 提交建议

这些 feature 依赖作者未提交的核心重构基线（Executor 接口、learning 模块、CLI 接线），
无法干净拆成独立 commit。建议作者审阅本清单后，将工作树（作者在途改动 + 上述 feature）
一并提交；如需拆分，按本清单逐文件提取本工作 hunk。构建已验证绿（gar_reason / gar_train_policy），
hybrid 探针 + 36 goals×3 seeds 全量评测通过。