# Local System One model service

`third_party/llm2jev` supplies the model runtime. GARAgent only sends HTTP
requests; it does not import Python, SGLang, or model weights.

For the architecture, request/response protocol, ranking formula, CLI,
measurement guidance, and troubleshooting, see
[docs/jev_guide.md](../../docs/jev_guide.md).

## Setup

```bash
git submodule update --init third_party/llm2jev
cd third_party/llm2jev
uv venv .venv-runtime
uv pip install --python .venv-runtime/bin/python --prerelease=allow -e '.[sglang]'
```

Download a supported model outside this repository. Start with
`Qwen/Qwen3-1.7B` for a latency probe. No model weights belong in the
submodule.

## Serve

From the GARAgent root directory:

```bash
JEV_VENV_BIN="$PWD/third_party/llm2jev/.venv-runtime/bin" \
JEV_MODEL_PATH=/path/to/Qwen3-1.7B deploy/jev/serve_local.sh
```

The service listens only on `127.0.0.1:30000` by default. Override
`JEV_HOST`, `JEV_PORT`, or `JEV_MODEL_NAME` when needed.

Check it from another terminal:

```bash
python3 deploy/jev/probe.py
```

The probe posts to `/v1/systemone` and prints the structured decision.

## GARAgent

```bash
./build/bin/gar_reason --mode agent \
  --csr /tmp/synth_csr --rules /tmp/synth_rules \
  --goal 'lives_in(p0,t1)' --budget 20 \
  --policy deterministic --scorer exact \
  --llm-guide --llm-backend jev \
  --llm-base-url http://127.0.0.1:30000 \
  --llm-model local-model \
  --verify-against-closure
```

`--llm-model` selects the model field in the System One request. The legacy
`--policy jev` path remains independent of this deployment interface.
