# JEV and LLM guidance

GARAgent can use a language model to order symbolic search candidates. The model never supplies logical evidence.

> **Rank, not invent.** Candidate generation, predicate evaluation, and fact derivation remain symbolic.

This guide covers the three model entry points, local LLM2Jev deployment, OpenAI-compatible services, caching, outputs, and troubleshooting.

## Entry points

| Entry point | When it runs | What the model orders | Typical use |
|---|---|---|---|
| `--llm-guide` | At most once per goal | Goal-reachable rule stars | Initial star ordering with a local or remote Choice model |
| `--paper-guide` | At most once per goal | Grounded actions through restricted priority rules | Paper-style cold-start guide, currently with OpenAI-compatible or mock clients |
| `--policy jev` | Potentially at every selection | Current top-k admissible motifs | Legacy per-step JevPolicy path |

These options are not interchangeable:

- `--llm-guide` changes star order; the local policy still selects motifs.
- `--paper-guide` compiles a restricted action-priority program; it does not return star probabilities.
- `--policy jev` is a separate policy and protocol path with different counters and defaults.

A local LLM2Jev Choice service supports `--llm-guide`. It does not currently implement the `Guide` request required by `--paper-guide`.

## Architecture

```text
gar_reason --mode agent
        |
        v
ReasoningAgent
  ├── Local Scorer / Policy / RelevanceModel
  |
  ├── LlmStarGuide             --llm-guide
  |       |
  |       v
  |   LlmClient::Choice
  |
  └── PaperGuide               --paper-guide
          |
          v
      LlmClient::Guide
          |
          v
      Restricted JSON compiler

LlmClient implementations
  ├── OpenAiLlmClient          DeepSeek / OpenAI-compatible chat completions
  ├── HttpJevClient            Local System One Choice service
  ├── CachedLlmClient          Real response reuse for PaperGuide
  └── MockLlmClient            Deterministic network-free validation
```

The C++ core communicates with model services over HTTP. Python, SGLang, and model weights remain outside the C++ build and outside MatrixGraph.

## Initial star guide

### Input

For each goal, `LlmStarGuide` constructs a request from:

- the grounded or open goal;
- graph vertex and edge counts;
- eligible rule IDs, names, descriptions, and confidence;
- consequent-to-goal dependency distance;
- a rendered dependency path;
- a mock-friendly numeric guide score.

Only rules whose consequent can reach the goal are sent to the model. This filter does not remove rules from symbolic execution. `--guide-max-options` limits model input, not the rule set used by the executor.

The request does not contain the full triple set, current ready set, or derived-fact journal. It expresses an initial directional preference, not a proof-state decision.

### Output and ranking

The Choice response contains one probability per option. GARAgent validates that probabilities are finite, nonnegative, aligned, and not all invalid. Valid preferences are divided by their maximum.

For `N` stars, a rule at baseline position `j` receives:

```text
baseline_r = (N - 1 - j) / (N - 1)
order_score_r = baseline_r + guide_strength * normalized_model_preference_r
```

Stars are stably sorted by decreasing `order_score`. A strength of zero disables the request. After star ordering, the configured local policy still selects grounded actions.

### Run with a mock

Add `--llm-mock` to any star-guide command:

```bash
./build/bin/gar_reason --mode agent --engine cpu \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --goal 'relation(a,b)' --budget 1000 \
  --policy deterministic --scorer exact --star-order goal \
  --llm-guide --llm-mock \
  --out /tmp/garagent-star-guide-mock
```

The mock uses rule confidence and a deterministic policy-seeded perturbation. It validates ranking, counters, fallback behavior, and soundness. It does not measure language-model quality.

## Paper guide

### Input

`PaperGuide` sends the goal, graph metadata, and up to 64 goal-reachable rule options. The options expose only registered features, including:

- rule confidence, support, and body size;
- consequent-to-goal distance;
- score total, price, and residual cost;
- witnessed and total evidence;
- grounded binding IDs, labels, and kinds when available.

The model receives option indices rather than arbitrary semantic aliases. This allows the compiler to map its response back to real rules.

### Output

The response is a restricted JSON object:

```json
{
  "rules": [
    {
      "rule": 0,
      "when": [
        ["rule.confidence", ">=", 0.8],
        ["goal.distance", "=", 1]
      ],
      "weight": 0.9
    }
  ]
}
```

Each entry contains:

- `rule`: an integer index from the supplied options;
- `when`: zero or more typed feature comparisons;
- `weight`: a finite value in `[-1,1]`.

The compiler accepts at most 16 priority entries from the model. Unknown features, invalid option indices, malformed conditions, and out-of-range weights are rejected. `==` is normalized to the compiler's `=` operator.

Guide priority is an optional first lexicographic key in the ready heap. The learned selection priority remains the next key. `--guide-lambda` controls how often guided selection is used for a batch; `1` always uses it and `0` disables it.

### Run with DeepSeek

```bash
export DEEPSEEK_API_KEY='your-key'

./build/bin/gar_reason --mode agent --engine cpu \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --goal 'relation(a,b)' --budget 1000 \
  --policy learned --scorer exact --star-order goal \
  --relevance-model /path/to/relevance.meta \
  --paper-guide --llm-backend openai \
  --llm-base-url https://api.deepseek.com \
  --llm-model deepseek-chat \
  --out /tmp/garagent-paper-guide
```

The key is read only when the client makes a request. Do not write real keys into source, documentation, command logs, or experiment manifests.

### Run with a fixed mock response

```bash
./build/bin/gar_reason --mode agent \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --goal 'relation(a,b)' --budget 1000 \
  --policy learned --relevance-model /path/to/relevance.meta \
  --paper-guide --llm-mock \
  --paper-guide-response /path/to/guide.json \
  --out /tmp/garagent-paper-guide-mock
```

The response still passes through the real compiler and symbolic checks. Mocking transport does not disable guide validation.

### Response cache

Use a cache directory to reuse a real response for the same goal:

```bash
./build/bin/gar_reason --mode agent \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --goal 'relation(a,b)' --budget 1000 \
  --policy learned --relevance-model /path/to/relevance.meta \
  --paper-guide --paper-guide-cache-dir /path/to/guide-cache \
  --llm-backend openai --llm-model deepseek-chat \
  --out /tmp/garagent-paper-guide-cached
```

On a miss, the client makes a real request and writes the response. On a hit, it reads the response without network access.

The cache key is based on the goal, not the complete rule set, prompt, or model configuration. Use different cache directories when those inputs change. Label cached measurements as warm-cache results; they are not cold-start measurements.

## Local LLM2Jev

LLM2Jev turns an open model into a structured Choice service. For a Choice request, it evaluates candidates with yes/no next-token logits instead of generating answer text:

```text
q = exp(yes_logit) / (exp(yes_logit) + exp(no_logit))
```

One HTTP request can score many candidates. It is not equivalent to one model forward pass. Latency depends on candidate count, prompt length, model size, hardware, initialization, and prefix-cache state.

### Install and start

Create an isolated runtime environment in the submodule:

```bash
git submodule update --init third_party/llm2jev
cd third_party/llm2jev
uv venv .venv-runtime
uv pip install --python .venv-runtime/bin/python \
  --prerelease=allow -e '.[sglang]'
```

Start the service from the GARAgent root with the repository wrapper:

```bash
JEV_VENV_BIN="$PWD/third_party/llm2jev/.venv-runtime/bin" \
JEV_MODEL_PATH=/path/to/model \
deploy/jev/serve_local.sh
```

The wrapper defaults to `127.0.0.1:30000` and `local-model`. Set `JEV_HOST`, `JEV_PORT`, or `JEV_MODEL_NAME` to override them. Check the service from another terminal with:

```bash
curl -fsS http://127.0.0.1:30000/health
python3 deploy/jev/probe.py --timeout 30
```

The probe sends a real two-option Choice request and checks both the protocol and model inference path. The first startup may include model loading, KV-cache allocation, and CUDA graph capture. A long-running service process is normal. See [the deployment README](../deploy/jev/README.md) and [the upstream usage guide](../third_party/llm2jev/docs/en/usage.md) for more options.

### Run the star guide with local JEV

```bash
./build/bin/gar_reason --mode agent --engine cpu \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --goal 'relation(a,b)' --budget 1000 \
  --policy deterministic --scorer exact --star-order goal \
  --llm-guide --llm-backend jev \
  --llm-base-url http://127.0.0.1:30000 \
  --llm-model local-model \
  --out /tmp/garagent-local-jev
```

No API key is required for this default local configuration. The C++ client currently supports plain `http://host:port` for this service. Do not expose an unauthenticated model service to an untrusted network.

`--llm-model` must equal `--served-model-name` from the service command.

## Wire protocols

### System One Choice

The local client sends a request to `/v1/systemone` with this general shape:

```json
{
  "model": "local-model",
  "include_choice_candidates": false,
  "state": "goal and graph metadata",
  "questions": {
    "pick": {
      "type": "choice",
      "instructions": "Rank real rule options without inventing facts.",
      "criteria": {
        "0": "rule metadata, confidence, distance, and path",
        "1": "another rule option"
      }
    }
  }
}
```

A response contains `answers.pick` with `choice`, `confidence`, and option-aligned `probabilities`. The client reconstructs an array in request-option order before applying preferences.

### OpenAI-compatible Choice and Guide

The OpenAI-compatible client uses chat completions and instructs the model to return JSON. Star Choice responses contain `choice`, `probabilities`, and `confidence`. Paper Guide responses contain the restricted `rules` object described above.

## CLI reference

| Option | Default | Meaning |
|---|---|---|
| `--llm-guide` | Off | Enable one initial star-guide request per goal |
| `--paper-guide` | Off | Enable the restricted action-level guide |
| `--policy jev` | Off | Select the legacy per-step JevPolicy |
| `--llm-backend openai\|jev` | `openai` | Select chat completions or System One |
| `--llm-base-url URL` | `https://api.deepseek.com` | Service base URL |
| `--llm-model NAME` | `deepseek-chat` | Remote or served model name |
| `--llm-timeout-ms N` | `120000` | Client request timeout |
| `--guide-strength F` | `1.0` | Star-guide fusion weight; zero disables the request |
| `--guide-max-options N` | `255` | Maximum star options sent to the model |
| `--guide-min-cost F` | `0` | Symbolic estimated-cost gate for star guidance |
| `--guide-lambda F` | `1.0` | Paper-guide usage probability in `[0,1]` |
| `--paper-guide-cache-dir PATH` | Empty | Read/write real PaperGuide responses |
| `--paper-guide-response PATH` | Empty | Fixed response for `--llm-mock` |
| `--llm-mock` | Off | Replace the real LLM client with a deterministic mock |
| `--policy-seed N` | `--seed` | Seed policy and mock randomness |

## Output fields

Star guidance writes `guide_overhead` in `summary.yaml`:

```yaml
guide_overhead:
  calls: 1
  wall_ms: 77.1
  fallbacks: 0
  reachable: 21
  options: 21
  active: true
  estimated_cost: 196
```

Paper guidance writes `paper_guide_overhead` and, when available, `paper_guide.json`:

```yaml
paper_guide_overhead:
  calls: 1
  wall_ms: 0.14
  fallbacks: 0
  rejected: 0
  active: true
```

Interpret the fields as follows:

- `calls`: attempted client calls, including failed requests;
- `wall_ms`: client and compilation overhead;
- `fallbacks`: requests that fell back to the local policy;
- `rejected`: rejected guide entries, which can occur without a total fallback;
- `reachable`: goal-reachable eligible stars;
- `options`: options actually sent to the model;
- `active`: whether a valid guide was activated;
- `estimated_cost`: symbolic pre-match estimate used only for gating.

The legacy per-step JevPolicy uses `control_overhead` instead.

Model wall time is included in end-to-end wall time but not in the symbolic action budget. Always compare goal hits, evaluated actions, derived facts, soundness, and total wall time together.

## Evaluation guidelines

Compare at least:

- no guide;
- the real online guide;
- a local model if relevant;
- an explicit warm-cache arm if repeated-goal execution matters.

Use mock only for control-flow validation. Keep the graph, rules, goals, relevance model, scorer, backend, and execution limits fixed across semantic comparisons.

Report service startup, unseen-goal requests, repeated requests, and training separately. A warm prefix cache or response cache must not be presented as cold-start performance. A reduction in reasoning steps does not prove an end-to-end speedup.

## Troubleshooting

### `Connection refused`

Check that the service is listening on the expected host and port. Review startup logs before sending experiment requests.

### `404: model ... does not exist`

Make `--llm-model` match the server's `--served-model-name`.

### `422` or a protocol parsing error

Confirm that the service implements `/v1/systemone` with `questions.pick.criteria` and `answers.pick`. Rebuild `gar_reason` after changing client code.

### `guide_overhead.fallbacks` is nonzero

Inspect the first fallback message on stderr. The run used the local fallback policy and cannot be used to claim model-ranking benefit.

### `paper_guide_overhead.rejected` is nonzero

Inspect `paper_guide.json`. The model may have used an unknown feature, an invalid option, an unsupported comparison, or an out-of-range weight. Rejected entries are not logical evidence.

### A cached run makes a real request

The cache missed. Check that the goal and cache directory match. The client creates a goal-hash filename inside the directory; do not rename it to a generic `response.json`.

### Local startup is slow or uses substantial GPU memory

Model loading, KV-cache allocation, and CUDA graph capture can dominate startup. GPU memory reservation does not imply continuous inference.

## Code index

| Path | Responsibility |
|---|---|
| `core/engine/agent/include/gar/core/agent/jev_client.h` | Client interfaces and result types |
| `core/engine/agent/src/jev_client.cpp` | HTTP, OpenAI, cache, mock, and response parsing |
| `core/engine/agent/include/gar/core/agent/guide.h` | Restricted guide types and feature catalog |
| `core/engine/agent/src/guide.cpp` | Typed compilation and priority evaluation |
| `core/engine/agent/src/guided_policy.cpp` | PaperGuide preparation and guided selection |
| `core/engine/agent/include/gar/core/agent/policy.h` | Policy, star-guide, and PaperGuide declarations |
| `tools/cli/agent_mode.h` | CLI parsing, component construction, and output |
| `third_party/llm2jev/` | Optional structured decision-model service |

When changing this module, preserve deterministic fallback, exact symbolic checks, explicit overhead accounting, and the separation between GARAgent and MatrixGraph.
