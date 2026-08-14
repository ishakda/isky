# Architecture Analysis & Integration Plan

*Phase 0 deliverable (prompt §55). Produced from direct inspection of the repository, not from its README.*

## 1. Architecture analysis of the existing K3 engine

### What the repository actually is

`kimi-k3-in-c` is a **kernel-level inference library plus a monolithic CLI driver**, not a
chat runtime. There is **no library-level `generate()` API**: the complete
prompt → tokenize → prefill → decode loop lives inside `main()` of `src/cli/k3_run.c`
(1,487 lines), including incremental KV-cache decode, session state save/load, n-gram
speculative decode, and hybrid draft-model decode.

### Public APIs (all under `include/k3/` and `src/*/**.h`)

| Layer | Header | Role |
|---|---|---|
| Kernels | `include/k3/k3.h` | `K3Cfg`, all math ops, `k3_decoder_layer_inc()` (one decoder layer, incremental or full), MXFP4 matmuls, scratch sizing |
| Config | `include/k3/k3_cfg.h` | `k3_cfg_load_file()` — reads the checkpoint's own `config.json` |
| Safetensors | `src/io/k3_st.h` | `k3_st_open/close` — shard index over the checkpoint directory |
| Binding | `src/model/k3_bind.h` | `k3_bind_layer()`, `k3_bind_model()` — resident weight binding (`K3LayerBind`, `K3ModelBind`) |
| Streamed trunk | `src/io/k3_trunk.h` | `k3_trunk_open/bind/prefetch/close` — layer-ordered streaming under a byte budget |
| Expert cache | `src/cache/k3_cache.h` | `k3_cache_init/free` — LRU MXFP4 expert cache exposing a `K3ExpertSrc` |
| Tokenizer | `src/tokenizer/k3_tok.h` + `third_party/tok.h` | `k3_tok_load()`, `tok_encode()`, `tok_decode()` — tiktoken-style BPE, header-only |

### How inference works (as implemented in `k3_run.c`)

1. `k3_cfg` loaded from checkpoint `config.json` (hardcoded fallback if absent).
2. `k3_st_open()` indexes shards; trunk either **resident** (`k3_bind_layer` per layer)
   or **streamed** (`k3_trunk_open` + per-layer `k3_trunk_bind`/`k3_trunk_prefetch`).
3. `k3_bind_model()` binds embed / final norm / lm_head. `k3_cache_init()` creates the
   expert cache; each layer's MoE weights get `moe.src = &cache->src` **at forward time**.
4. A local `forward()` (~115 lines) embeds ids, runs `k3_decoder_layer_inc` over all
   bound layers (with per-MLA-layer KV cache slices in incremental mode), applies the
   model-level AttnRes aggregator, final RMSNorm, lm_head → logits.
5. Decode is **greedy argmax only**. Incremental mode carries KDA recurrent state +
   MLA KV cache; `k3_state_save/load` (static functions in the CLI) persist sessions.
6. Errors: `forward()` returns -1; `k3_expert_drops` global must be checked post-run.

### Build configuration

CMake ≥3.16, C99 (`-std=gnu99` semantics via GCC defaults), targets: `k3` (static lib),
`k3_cli` (output name `k3`), 9 unit tests + `k3_model` oracle gate, `bench_kernels`.
Tests run against a **tiny fixture checkpoint** (`tests/fixtures/tiny_k3.{bin,json}`,
`tests/fixtures/st/*.safetensors`) — full agent development requires no real checkpoint.
OpenMP optional; pthreads required (trunk reader thread). Sanitizer option `K3_SANITIZE`.

### Platform-specific code

- `src/io/k3_portable_io.h` isolates `pread`/Windows `ReadFile` equivalents.
- `k3_run.c` uses `/proc/meminfo` (Linux) with graceful fallback, `getrusage`.
- Trunk streaming uses pthreads. Everything else is portable C99.

## 2. Integration plan

### Safest integration point

**Do not touch `src/cli/k3_run.c`.** Its `forward()`, `Weights`, and state save/load are
`static` and interleaved with CLI reporting; extracting them would risk regressions in the
oracle-validated path for zero architectural gain. Instead the agent gets its own
**engine session module** (`agent/model/k3_engine.c`) built on the same public APIs
(`k3_st` + `k3_bind`/`k3_trunk` + `k3_cache` + `k3_decoder_layer_inc` + tokenizer). The
~120 duplicated lines of forward-pass logic are a deliberate trade: the CLI stays
byte-identical, and the agent session gains what the CLI never had — sampling
(temperature/top-p), stop sequences, streaming token callbacks, and multi-turn reuse of a
loaded model across many generations in one process (the CLI loads, decodes once, exits).

The agent then talks **only** to a `ModelBackend` vtable (`agent/model/model_interface.h`);
`k3_backend.c` adapts the session to it. A deterministic `MockBackend` implements the same
vtable for fast tests of the entire agent stack.

### Files to create (all new, under `agent/`, `config/`, `tests/agent/`, `docs/`)

Per the structure in the build prompt §4: `agent/core` (agent, loop, task, state),
`agent/model` (interface, k3_engine, k3_backend, mock_backend), `agent/planner`,
`agent/tools` (registry, filesystem, shell, code, web, git), `agent/memory`,
`agent/reasoning` (reflection, verification), `agent/security` (permissions, sandbox),
`agent/context`, `agent/storage` (SQLite), `agent/api` (HTTP server), `agent/util`
(log, buf, json helpers, platform), `agent/prompts/system.txt`, `agent/main.c`,
`config/*.json`, `tests/agent/*`, `docs/*.md`.

### Files to modify

- `CMakeLists.txt` — add `sqlite3`, `cjson`, `agent_core` lib, `k3-agent`, `k3-agent-tests`
  targets. Existing targets untouched.
- Nothing else.

### Files that must NOT be modified

Everything under `src/`, `include/`, `third_party/tok*.h`, `third_party/json.h`,
`tests/unit/`, `tests/fixtures/`, `cmake/`, `benchmarks/`, `tools/`, `scripts/`.
Gate: all 10 existing ctest entries must keep passing at every phase.

### Dependencies (vendored, zero external runtime deps)

- `third_party/cJSON.{c,h}` v1.7.18 (MIT) — strict JSON for the tool-call protocol
  (`third_party/json.h` is a 175-line config reader, insufficient for validation/repair).
- `third_party/sqlite/sqlite3.{c,h}` 3.50.4 (public domain) — persistence.

### Risks

1. **Base-model JSON reliability** — Kimi K3 base emits free text. Mitigation: few-shot
   structured prompting, JSON extraction/repair, regeneration retry, simplified fallback
   plan format; runtime-side validation is the source of safety, never the model.
2. **Real-checkpoint validation infeasible in CI** (113 GB, ~40 s/token). Mitigation:
   tiny fixture checkpoint + MockBackend; real-model runs are a user-side option.
3. **Engine session duplication drift** — if upstream changes `forward()` semantics.
   Mitigation: `test_k3_engine` gates the session against the tiny-model oracle tokens.
4. **Windows** — process spawning/sandboxing isolated behind `agent/util/platform.h`;
   POSIX implemented first, Win32 stubs compile-guarded.
5. **Context ceiling** — K3_MAX_PROMPT 32768 ids; agent context builder budgets under it
   and compresses.

### Implementation order

Phases 1–8 as specified in the build prompt §53, each phase compiling + tested before
the next: backend abstraction & CLI → tools & JSON protocol → planner/task/verify/recover
→ SQLite memory & context → coding/git/web → security hardening → API/events/multi-agent
→ tests/benchmarks/docs.
