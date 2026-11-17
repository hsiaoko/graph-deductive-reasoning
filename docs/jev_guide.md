# Jev / LLM 决策指引模块

本文说明 GARAgent 中 Jev 与通用 LLM 客户端的用途、原理、部署、调用方式和实验注意事项。阅读本文后，应能独立启动本地模型服务、运行带指引的 agent、解释输出，并在修改代码时保持符号推理的正确性边界。

## 1. 这个模块解决什么问题

AdaDeduce 在一个 goal 下可能有许多可执行的规则 star 和 motif。不同执行顺序不会改变规则的逻辑含义，但会显著影响：

- 多快推出 goal；
- 执行多少次匹配和精确求值；
- 产生多少与 goal 无关的中间事实；
- 总墙钟时间。

Jev / LLM 模块利用规则元数据、goal 和依赖图信息，为候选提供软排序偏好。核心原则是：

> **Rank, not invent：模型只排序，不创造事实、规则或证明。**

候选仍由符号系统生成，谓词仍由 CPU/GPU 精确求值，派生仍由规则执行器完成。模型输出只影响搜索顺序，不是逻辑证据。

## 2. 两套入口不要混淆

当前代码保留两套不同用途的入口。

| 入口 | 调用时机 | 模型排序对象 | 当前定位 |
| --- | --- | --- | --- |
| `--llm-guide` | 每个 goal 启动时最多一次 | 所有 eligible 规则 star | 推荐的初始指引接口，可与 deterministic、learned 等 policy 组合 |
| `--policy jev` | ready-set 非空时，可能每步调用 | 当前 top-k admissible motif | 旧的逐步决策 policy，对应论文中的 JevPolicy 路径 |

二者共享 `LlmClient` 抽象，但调用频率、输入、协议默认值和统计字段不同。

本地 LLM2Jev 服务推荐使用第一种：

```text
--policy deterministic --llm-guide --llm-backend jev
```

`--policy jev` 默认走旧 `/systemOne` 接口，而本文的本地服务走 `/v1/systemone`。不要因为名字相同就混用两套参数。

## 3. 总体架构

```text
gar_reason --mode agent
        |
        v
MakeReasoningAgent
        |
        +-- 本地 policy：deterministic / learned / ...
        |
        +-- LlmStarGuide（启用 --llm-guide 时）
                |
                v
            LlmClient::Choice
                |
                +-- HttpJevClient ---- HTTP ----> llm2jev-serve
                |                                  |
                |                                  +-- SGLang
                |                                  +-- 本地 Qwen 等开源模型
                |
                +-- OpenAiLlmClient -- HTTPS ----> DeepSeek / OpenAI 兼容服务
                |
                +-- MockLlmClient（无网络、确定性回归）

LlmStarGuide 返回规则偏好
        |
        v
DeduceSession::BuildPlans 重排规则 star
        |
        v
AdaDeduceExecutor + 本地 policy + CPU/GPU 精确匹配与求值
```

职责边界：

- `third_party/llm2jev`：独立上游 submodule，负责把开源 LLM 转成结构化决策服务。
- `deploy/jev`：本仓库的本地启动、健康检查和部署说明。
- `core/engine/agent`：构造状态、调用客户端、校验概率、应用排序和失败回退。
- MatrixGraph：继续提供通用图匹配和 CUDA 组件；Jev 指引不要求新增 `GARAgentMatch`，也不改变全量 chase 的行为。

GARAgent 的 C++ core 不导入 Python、SGLang 或模型权重，进程间只通过 HTTP 通信。

## 4. 初始 star 指引的完整流程

### 4.1 构造 eligible star 集合

`DeduceSession::BuildPlans()` 先为规则构造执行计划，只有 `BuildRulePlan()` 成功的规则进入候选集合。基础顺序由 `--star-order` 决定：

- `input`：保持输入规则顺序；
- `goal`：按 consequent 到 goal 的依赖图距离排序。

模型只接收 consequent 在依赖图中能到达 goal 的 star。为适配 System One 单个 Choice 最多 255 个 criteria 的限制，默认最多取基础顺序中的前 255 个相关 star；`--guide-max-options` 可进一步降低上限，未送入模型的规则偏好为 0。`summary.yaml` 的 `guide_overhead.reachable` 和 `guide_overhead.options` 分别记录相关 star 数和实际送模数。

这个过滤与上限只作用于模型输入，**不会从符号执行中删除任何规则**。

可选的符号成本门控使用关系基数和平均度估计 star 的 join 成本，不做完整匹配。`--guide-min-cost F` 设置启用 guide 的最低总估计成本，默认 0，保持原先“有候选就调用”的行为。`guide_overhead.estimated_cost` 和 `active` 记录估计值及门控结果；门控关闭时 calls=0，所有偏好保持 0。

使用 `--policy adaptive --value-model <numeric-model> --llm-guide` 时，门控关闭走 Deterministic，门控开启才走 numeric-Q + Jev。估计只影响策略与排序，不是逻辑可满足性证明。

### 4.2 每个 goal 最多请求一次

`LlmStarGuide::Preferences()` 第一次被调用时构造请求，之后缓存本 goal 的偏好向量，不重复调用。

请求包含：

- goal 的关系名和两个参数，未绑定参数显示为 `_`；
- 图的顶点数和边数；
- 每个送入模型的规则的 ID、名称、描述和 confidence；
- consequent 到 goal 的依赖距离 `d`；
- 依赖图渲染出的推导路径；
- `guide_score`，当前取自规则 confidence，主要供 mock 客户端使用。

请求不包含完整三元组、当前 ready-set、具体 motif embedding 或已派生事实列表。因此它表达的是“起步时哪些规则方向值得优先尝试”，不是对当前证明状态的逐步判断。

### 4.3 校验并归一化模型输出

客户端返回与选项对齐的概率。GARAgent 会检查：

- 概率数量必须与选项数量一致；
- 每个概率必须有限且非负；
- 不能全部为零；若全部为零但给出了合法 `choice`，只将该选项置为 1；
- 失败时将全部偏好清零并回退到基础顺序。

合法概率随后除以最大值，使最高偏好为 1。这里使用的是**最大值归一化**，不是再次按总和归一化。`confidence` 当前不参与 star 排序。

### 4.4 与基础排序融合

假设当前共有 `N` 个 star，某规则在基础顺序中的下标为 `j`，其模型偏好为 `p_r`，则：

```text
baseline_r = (N - 1 - j) / (N - 1)
preference_r = p_r / max(p)
order_score_r = baseline_r + guide_strength * preference_r
```

代码按 `order_score_r` 降序稳定排序。`--guide-strength` 对应融合权重：

- `0`：不请求模型，只保留基础顺序；
- 默认值 `1.0`：基础顺序和模型偏好共同作用；
- 更大值：提高模型偏好的影响，但不会绕过任何符号约束。

### 4.5 后续仍由本地 policy 和精确执行器负责

star 顺序不等于最终 motif 选择。后续 ready-set 中的 motif 仍由 `--policy` 指定的策略选择，所有匹配、挂起、复活、谓词检查与派生仍由 AdaDeduce 执行。

因此，模型排序可能改变步数和耗时，但不能直接把一个模型建议写成新事实。

## 5. HTTP 协议：输入和输出是什么

### 5.1 请求

本地客户端向 `/v1/systemone` 发送一个 Choice 问题。下面是简化示例，实际 rule ID 使用规则在输入向量中的数字下标：

```json
{
  "model": "local-model",
  "include_choice_candidates": false,
  "state": "goal: lives_in(p0,t1)\ngraph vertices: 15 edges: 23\nThis is the only guide request for this goal.",
  "questions": {
    "pick": {
      "type": "choice",
      "instructions": "Rank the rule stars ... Do not invent facts or rules ...",
      "criteria": {
        "16": "rule gar_000016: ...\nconfidence=0.900\nd=0\npath: ...",
        "0": "rule gar_000000: ...\nconfidence=0.800\nd=1\npath: ..."
      }
    }
  }
}
```

当前 LLM2Jev 协议字段是 `criteria`，不是 `options`。GARAgent 用数字 rule index 作为不透明 option key，以便把返回概率准确映射回规则。本地请求设置 `include_choice_candidates=false`，避免每个候选的评分输入都重复携带完整候选清单；当前候选自身的描述仍保留，所有选项也仍分别评分。

### 5.2 响应

```json
{
  "model": "local-model",
  "answers": {
    "pick": {
      "type": "choice",
      "choice": "16",
      "confidence": 0.72,
      "probabilities": {
        "16": 0.86,
        "0": 0.14
      }
    }
  },
  "usage": {
    "input_tokens": 1868,
    "output_tokens": 0
  }
}
```

`HttpJevClient` 解包 `answers.pick`，再按请求选项顺序重构概率数组。旧的顶层 `choice/probabilities` 响应仍保留兼容解析。

### 5.3 OpenAI 兼容后端的差异

`--llm-backend openai` 使用 chat completions。模型被要求在消息中生成包含 `choice`、`probabilities`、`confidence` 的 JSON，C++ 再解析该 JSON。

这与本地 LLM2Jev 不同：后者不让模型生成 JSON，而是直接读取 yes/no logits。两者最终都适配为同一个 `LlmChoiceResult`。

## 6. 本地 LLM2Jev 的原理

本地部署的是 **LLM2Jev + 开源模型**，不是官方 Jev 的私有权重或服务。当前推荐用 Qwen3-1.7B 做延迟与集成验证；模型能力提升不等于排序效果必然提升，需要单独评测。

### 6.1 Prefill-only，不生成答案文本

对于一个 Choice 请求，LLM2Jev 会把每个候选分别改写成“该候选是否是最佳答案”的 yes/no 判断。模型处理输入后，直接读取下一 token 的 `yes` 和 `no` logits：

```text
q = exp(yes_logit) / (exp(yes_logit) + exp(no_logit))
```

程序再对各候选的 `q` 做归一化，得到 Choice 概率分布。模型不生成 yes/no 文本、概率数字或响应 JSON，所以 `usage.output_tokens` 通常为 0。

### 6.2 一次 HTTP 请求不等于一次模型前向

若一个 goal 实际送入 `N` 个 star（`guide_overhead.options`，当前最多 255），则一次 HTTP Choice 请求内部包含 `N` 个独立 yes/no 评分。它仍然需要真实模型计算，成本取决于：

- 候选数量；
- state、instructions 和 criteria 的 token 长度；
- 模型大小与硬件；
- 是否命中共享前缀缓存；
- 首次 CUDA graph 捕获或内核初始化是否已完成。

这正是“每 goal 一次调用”仍可能出现较高延迟的原因。

### 6.3 分阶段提交与 Radix Cache

SGLang 后端的 LLM2Jev 默认采用 staged submission：

1. 先提交一个真实候选，建立公共 token 前缀的 KV cache；
2. 后续候选复用相同前缀，仅计算分叉部分；
3. 缓存由 SGLang Radix Cache 管理，可跨候选、也可能跨请求保留，但可能被驱逐。

需要区分三种“慢”：

- 服务启动慢：加载权重、分配 KV pool、捕获 CUDA graph；
- 新输入冷请求慢：相关前缀尚未建立，或仍有首次内核成本；
- 完全相同的请求很快：历史前缀已命中。

最后一项不能代表所有新 goal 的稳定性能。缓存匹配依据完整分词后的实际前缀，语义相似或字段相同不保证命中。

## 7. 本地部署与启动

### 7.1 准备环境

要求：Python 3.10+、`uv`、可用 CUDA GPU/驱动，以及放在仓库外的模型目录。

```bash
cd /home/zhuxk/project/graph/GARAgent
git submodule update --init third_party/llm2jev
cd third_party/llm2jev
uv venv .venv-runtime
uv pip install --python .venv-runtime/bin/python \
  --prerelease=allow -e '.[sglang]'
```

使用独立 `.venv-runtime` 可以避免继承系统环境中不匹配的 `flashinfer-cubin`。不要通过设置 `FLASHINFER_DISABLE_VERSION_CHECK=1` 绕过版本检查。

### 7.2 启动服务

回到仓库根目录：

```bash
cd /home/zhuxk/project/graph/GARAgent
CUDA_VISIBLE_DEVICES=1 \
JEV_VENV_BIN="$PWD/third_party/llm2jev/.venv-runtime/bin" \
JEV_MODEL_PATH=/path/to/Qwen3-1.7B \
deploy/jev/serve_local.sh
```

`CUDA_VISIBLE_DEVICES=1` 只是本机使用空闲 GPU 1 的示例，应按实际资源调整。模型路径可使用本机已有的 `/ssd_data/zhuxk/models/Qwen3-1.7B`。

服务是常驻进程，持续显示 `running` 是正常状态，不代表一直在推理。首次启动可能需要数分钟捕获 CUDA graph。SGLang 还会预分配 KV cache 和图缓冲，因此显存占用会明显大于模型权重。

可用环境变量：

| 变量 | 默认值 | 用途 |
| --- | --- | --- |
| `JEV_MODEL_PATH` | 无，必填 | 本地模型目录 |
| `JEV_VENV_BIN` | `third_party/llm2jev/.venv/bin` | Python 环境的 `bin` 目录；本文显式指向 `.venv-runtime/bin` |
| `JEV_HOST` | `127.0.0.1` | 监听地址，默认仅本机可访问 |
| `JEV_PORT` | `30000` | 服务端口 |
| `JEV_MODEL_NAME` | `local-model` | 对外模型名，必须与 GARAgent 的 `--llm-model` 一致 |
| `CUDA_VISIBLE_DEVICES` | 继承环境 | 选择服务可见的 GPU |

### 7.3 健康检查

```bash
curl -fsS http://127.0.0.1:30000/health
python3 deploy/jev/probe.py --timeout 30
```

`probe.py` 发送一个真实的两选项 Choice 请求并打印结构化响应。它不仅检查端口，还检查 `/v1/systemone` 协议和模型推理链路。

## 8. 在 GARAgent 中运行

先构建客户端：

```bash
cmake --build build --target gar_reason -j4
```

### 8.1 无模型基线

```bash
./build/bin/gar_reason --mode agent --engine gpu \
  --csr /tmp/synth_csr --rules /tmp/synth_rules \
  --goal 'lives_in(p0,t1)' --budget 20 \
  --policy deterministic --scorer exact --star-order input \
  --out /tmp/agent_baseline \
  --verify-against-closure --print 0
```

### 8.2 本地 Jev 指引

```bash
./build/bin/gar_reason --mode agent --engine gpu \
  --csr /tmp/synth_csr --rules /tmp/synth_rules \
  --goal 'lives_in(p0,t1)' --budget 20 \
  --policy deterministic --scorer exact --star-order input \
  --llm-guide --llm-backend jev \
  --llm-base-url http://127.0.0.1:30000 \
  --llm-model local-model \
  --guide-strength 1.0 \
  --out /tmp/agent_local_jev \
  --verify-against-closure --print 0
```

本地 Jev 不需要 API key。当前 `HttpJevClient` 只支持明文 `http://host:port`，适用于本机或受控内网，不支持 `https://`。

### 8.3 DeepSeek / OpenAI 兼容服务

```bash
export DEEPSEEK_API_KEY='your-key'
./build/bin/gar_reason --mode agent --engine gpu \
  --csr /tmp/synth_csr --rules /tmp/synth_rules \
  --goal 'lives_in(p0,t1)' --budget 20 \
  --policy deterministic --scorer exact --star-order input \
  --llm-guide --llm-backend openai \
  --llm-base-url https://api.deepseek.com \
  --llm-model deepseek-chat \
  --out /tmp/agent_deepseek \
  --verify-against-closure --print 0
```

密钥只在调用时从 `DEEPSEEK_API_KEY` 读取。不要把真实密钥写入源码、文档、命令记录或实验输出。

### 8.4 无网络 mock

在上述任一 guide 命令中加入：

```bash
--llm-mock
```

此时不会访问网络。`MockLlmClient` 使用 criteria 中的 `guide_score`（当前为规则 confidence）和由 `--policy-seed` 控制的确定性扰动生成偏好。

Mock 适用于验证：

- 每个 goal 的调用次数；
- 概率对齐与 star 重排；
- 输出统计；
- 符号 soundness；
- 服务不可用之外的控制流回归。

它不具备语言模型语义判断能力，不能用于证明 LLM 指引有效。

## 9. CLI 参数速查

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--llm-guide` | 关闭 | 启用每 goal 一次的初始 star 指引 |
| `--llm-backend openai\|jev` | `openai` | 选择 chat completions 或 System One 客户端 |
| `--llm-base-url URL` | `https://api.deepseek.com` | 服务地址；本地 Jev 通常填 `http://127.0.0.1:30000` |
| `--llm-model NAME` | `deepseek-chat` | 模型名；本地必须与 `JEV_MODEL_NAME` 一致 |
| `--llm-timeout-ms N` | `120000` | 单次客户端请求超时 |
| `--guide-strength F` | `1.0` | 非负融合权重；为 0 时不请求模型 |
| `--guide-max-options N` | `255` | 最多送入模型的 goal-reachable star 数 |
| `--guide-min-cost F` | `0` | 符号估计成本的调用门控；adaptive 在关闭时退回 Deterministic |
| `--llm-mock` | 关闭 | 用确定性 mock 替换真实客户端 |
| `--star-order input\|goal` | `input` | 模型偏好融合前的基础 star 顺序 |
| `--policy-seed N` | 继承 `--seed` | mock 和其他 policy 随机数的种子 |
| `--verify-against-closure` | 关闭 | 检查 agent 派生事实是否都属于全量闭包 |

## 10. 输出与统计如何解读

`summary.yaml` 中与 guide 直接相关的字段：

```yaml
llm_guide: true
guide_strength: 1
llm_backend: jev
llm_base_url: http://127.0.0.1:30000
llm_model: local-model
steps: 5
evaluate_events: 5
prepare_wall_ms: 391.877
guide_overhead:
  calls: 1
  wall_ms: 77.1357
  fallbacks: 0
  reachable: 21
  options: 21
  active: true
  estimated_cost: 196
soundness: pass
```

字段含义：

- `calls`：实际客户端请求尝试数，包括失败请求；正常情况下每 goal 至多为 1。
- `wall_ms`：模型客户端控制开销，包含网络和服务端评分时间。
- `fallbacks`：协议、连接、超时或概率校验失败后回退的次数。
- `reachable`：依赖图中 consequent 能到达 goal 的 eligible star 数。
- `options`：实际送入模型的 star 数，由 `--guide-max-options` 限制。
- `active`：符号成本门控是否允许本次指引。
- `estimated_cost`：基于关系基数与平均度的预匹配成本估计，仅用于门控。
- `prepare_wall_ms`：准备阶段耗时，包含初始 guide。
- `steps` / `evaluate_events`：观察搜索是否真正减少，而不能只看模型概率。
- `goal_derived`：预算内是否推出 goal。
- `soundness`：仅在启用 `--verify-against-closure` 时出现，表示派生事实均属于精确闭包。

模型调用时间计入墙钟，但不消耗 AdaDeduce 的逻辑预算 `B`。soundness 通过不代表搜索最快，也不代表所有可推出 goal 都一定能在有限预算内命中。

旧 `--policy jev` 的统计字段是 `control_overhead`，不是 `guide_overhead`。

## 11. 当前小图验证结果与边界

在同一个 15 顶点、23 边、21 规则的合成图上，goal 为 `lives_in(p0,t1)`，已得到以下 smoke 结果：

| 方法 | 步数 | 模型调用耗时 | 端到端耗时 |
| --- | ---: | ---: | ---: |
| 无 guide | 8 | — | 0.748 s |
| DeepSeek | 5 | 1.193 s | 1.954 s |
| 本地 Jev，首次长提示 | 5 | 7.915 s | 8.263 s |
| 本地 Jev，同一请求热缓存复跑 | 5 | 0.077 s | 0.415 s |

这些运行都推出 goal、无 guide fallback，且 soundness 通过。结果说明：

1. 指引确实能在这个例子上把 8 步降为 5 步。
2. 更少推理步骤不保证端到端更快，模型开销可能抵消收益。
3. 本地 prefill-only 不自动等于低延迟，候选数和输入长度仍很重要。
4. 0.415 秒是**同请求热缓存复跑**，不能外推到未见 goal、FB15K-237 或更大规则集。

对应输出位于 `.work_hdcv/llm_small/`；该目录是本地实验产物，不是稳定测试数据接口。

### FB15K-237 rules640 / 36-goal 完整对照

正式矩阵使用冻结的 640 条规则和 36 个测试 goal（round1、round2、deep 各 12 个），两臂均为当前 GPU AdaDeduce、deterministic policy、exact scorer、goal star order、budget `1e9`、candidate cap `100000`、fair interval 0。每臂执行 seeds `{7,13,42}`，共 216 runs，逐 goal 取三 run 中位数后再宏平均。

| 方法 | 外部 wall(s) | 内部 wall(s) | 求值 | guide(s) |
| --- | ---: | ---: | ---: | ---: |
| Deterministic | 11.035 | 9.852 | 46,637.1 | 0 |
| Deterministic + local Jev | 11.187 | 9.969 | 46,649.6 | 0.123 |

结果为 baseline/Jev wall `0.986x`，即 Jev 宏观外部墙钟约慢 1.38%；求值增加 0.027%。Jev 仅在 4/36 goals 上取得中位墙钟优势，在 16/36 goals 上减少求值。矩阵首轮的 wall 加速为 `0.883x`，重复请求为 `0.987x`，说明缓存降低了模型开销，但没有改变整体无端到端收益的结论。

216/216 runs 均推出 goal、soundness 通过、无 timeout、无 guide fallback；baseline/Jev 分别有 27/21 个 run 触发 candidate cap，因此状态为 `complete_with_truncation`，不宣称无截断的全局完备性。两臂都只有 2/36 goals 的三次 chain 逐字节一致，微小的跨 run 求值差异不能全部归因于模型。

分层看，round1 求值减少 1.716%，round2 增加 2.700%，deep 只减少 0.023%，三层宏观墙钟均未获益。另做 `p00` 清缓存复核时，求值减少 15.942%，但 guide 为 3.919 秒，外部 wall 从 1.215 秒增至 5.082 秒，直观展示了“搜索减少但模型成本抵消收益”。

完整结果见 `.work_hdcv/fb15k_jev640/summary.md`、`summary.json` 和 `paired.tsv`；清缓存单点见 `.work_hdcv/fb15k_jev640_cold_p00/`。当前证据不支持在此设置下默认启用该 Qwen3-1.7B guide，但不排除更强的任务特定指引、更低延迟模型或 LLM→ML warm-start 的价值。

## 12. 怎样设计有说服力的对照实验

至少固定相同的图、规则、goal 集合、policy、scorer、budget 和随机种子，比较：

- 无 guide；
- 在线 chat LLM guide；
- 本地 Jev guide；
- 如需要，再加入 mock 仅作控制流 sanity check，不纳入语义效果排名。

每个 goal 应记录：

- goal 命中率、步数、精确求值次数、派生事实数；
- 总墙钟、guide 墙钟、fallback 次数；
- soundness；
- 是否新 goal、是否完全重复请求、服务是否刚启动。

建议分别报告：

1. **服务启动成本**：权重加载和 CUDA graph 初始化。
2. **未见输入的冷 guide**：更接近实际新 goal。
3. **重复请求的热 guide**：只用于分析缓存上限。
4. **总生命周期成本**：模型服务、训练、冷启动和后续推理一起计算。

不要只报告“总耗时减 guide 耗时”作为真实加速；它只是粗略的推理侧估算，缓存和调度可能产生耦合。上游也说明 staged HTTP 服务的并发性能尚未充分验证，做延迟对照应优先串行请求，并单独评测并发。

FB15K-237 的 rules640 / 36-goal 矩阵可直接复现：

```bash
CUDA_VISIBLE_DEVICES=2 python3 experiments/llm_guide/run_fb15k_jev.py --engine gpu --phase all

# numeric-Q 与 numeric-Q + Jev 的同协议组合对照
CUDA_VISIBLE_DEVICES=2 python3 experiments/llm_guide/run_fb15k_jev.py --engine gpu \
  --work .work_hdcv/fb15k_jev640_numeric \
  --base-policy learned \
  --value-model .work_hdcv/fixpoint_q640/models/q_numeric.meta \
  --phase all
```

默认输出为 `.work_hdcv/fb15k_jev640/`。脚本固定二进制、goals、rule IDs 和完整闭包哈希，支持断点续跑；两臂都使用当前 GPU AdaDeduce，不能把这个新基线与历史 CPU Fixpoint 墙钟直接混比。正式矩阵为 216 runs，seed13/42 的模型请求与 seed7 相同，因此脚本另列矩阵首轮和重复请求。使用 learned 基线时，value model 的路径和哈希也会写入 manifest。

## 13. LLM 冷启动后切换到 ML

`experiments/fixpoint_policy/run_warm_start.py` 实现了另一层组合：

1. 前 N 个 cold goals 使用 LLM star guide，同时采集 Q-trace；
2. 用这些 trace 训练 numeric 或 HDC value model；
3. 后续 warm goals 切换到 `--policy learned`，不再调用 LLM；
4. 汇总推理、模型调用和训练的生命周期成本。

示例：

```bash
python3 experiments/fixpoint_policy/run_warm_start.py \
  --work /tmp/garagent-warm-start \
  --cold-goals 4 --engine gpu --model numeric \
  --llm-backend jev --llm-base-url http://127.0.0.1:30000 \
  --llm-model local-model
```

快速验证流程可加 `--llm-mock`，但它同样不能证明真实 LLM 的语义收益。该方案的目标是把一次性或少量 LLM 指引转化为可复用的本地策略，而不是让每个 goal 都承担在线模型成本。

## 14. 常见问题与排查

### `Connection refused`

服务尚未监听或已退出。先检查启动日志和 `/health`，模型加载期间不要盲目连续发送实验请求。

### `404: model ... does not exist`

`--llm-model` 与服务端 `JEV_MODEL_NAME` 不一致。二者都设为 `local-model` 是最简单的本地配置。

### `422`，提示 `options` 不允许或 `criteria` 缺失

请求仍使用旧协议字段。当前客户端应发送 `questions.pick.criteria`，并解析 `answers.pick`。重新构建：

```bash
cmake --build build --target gar_reason -j4
```

### `flashinfer-cubin version ... does not match`

系统 Python 包与 SGLang/flashinfer 版本混用。使用本文的 `.venv-runtime` 隔离环境，并确认 `JEV_VENV_BIN` 指向它；不要禁用版本检查。

### `guide_overhead.fallbacks` 非零

查看 stderr 中第一条 `[guide] fallback: ...`。此时偏好全部清零，搜索退回基础 star 顺序；逻辑正确性仍由符号执行保证，但这次运行不能用于评价模型排序收益。

### 本地服务启动慢、显存占用高

首次 CUDA graph 捕获可能需要数分钟；SGLang 会预分配 KV pool 和运行缓冲。显存占用不等于模型权重的大小，也不代表正在持续推理。常驻服务在任务面板中长期显示 `running` 是预期行为。

### 本地 Jev 支持 HTTPS 吗

当前 C++ `HttpJevClient` 不支持。默认只绑定 `127.0.0.1`，不要未经授权把无认证的模型服务暴露到公网。

### 能否让模型直接输出新事实或证明

当前架构不允许，也不建议这样做。若要扩展，仍必须让符号系统重新验证候选事实和完整推导，不能把文本答案直接写入图。

## 15. 代码索引与修改约定

| 路径 | 职责 |
| --- | --- |
| `core/engine/agent/include/gar/core/agent/jev_client.h` | 通用 `LlmClient`、结果结构和三种客户端声明 |
| `core/engine/agent/src/jev_client.cpp` | HTTP、OpenAI、JSON 编解码、概率对齐和 mock |
| `core/engine/agent/include/gar/core/agent/policy.h` | `LlmStarGuide` 与旧 `JevPolicy` 接口 |
| `core/engine/agent/src/policy.cpp` | 状态构造、每 goal 缓存、概率校验、归一化和 fallback |
| `core/engine/agent/src/deduce_session.cpp` | `BuildPlans()` 中融合基础顺序与模型偏好 |
| `tools/cli/agent_mode.h` | CLI 参数、客户端组装、`summary.yaml` 统计 |
| `core/engine/agent/CMakeLists.txt` | C++20、vendored JSON、CURL、CUDA 和 MatrixGraph 链接 |
| `third_party/llm2jev` | 上游结构化决策与本地模型运行时 |
| `deploy/jev/serve_local.sh` | 本地 SGLang 服务启动 |
| `deploy/jev/probe.py` | 真实 System One 健康请求 |
| `experiments/llm_guide/run_fb15k_jev.py` | FB15K-237 rules640、36 goals、三 seed 的可恢复对照与闭包对账 |
| `experiments/fixpoint_policy/run_warm_start.py` | LLM 冷启动、Q-trace、ML warm-start 流程 |

修改时应保持：

- 模型只排序，符号系统负责候选生成和全部逻辑检查；
- 不改变全量 chase 的语义和默认行为；
- 模型失败必须有确定性的本地回退；
- 控制开销必须记录，不能藏进逻辑预算；
- 不为 core 引入 Python/SGLang 构建依赖；
- 优先在上游之外做适配，确需修改 `third_party/llm2jev` 时遵循其 `AGENTS.md` 并运行上游测试；
- 新增或修改协议时用 mock 和真实服务分别验证，不只检查能否编译。

上游原理参考：

- `third_party/llm2jev/docs/zh/request-to-model.md`
- `third_party/llm2jev/docs/zh/shared-prefix-cache.md`
- `third_party/llm2jev/docs/zh/usage.md`
