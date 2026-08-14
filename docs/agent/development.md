# Development

## Building

```sh
cmake -B build
cmake --build build -j
```

`agent/agent.cmake` is included from the top-level `CMakeLists.txt`
(`include(${CMAKE_CURRENT_SOURCE_DIR}/agent/agent.cmake)`) and adds, on top of the
existing K3 targets:

| Target | Type | Notes |
|---|---|---|
| `agent_cjson` | static lib | vendored `third_party/cJSON.c` |
| `agent_sqlite` | static lib | vendored `third_party/sqlite/sqlite3.c`, built with `-w` (the amalgamation's own warnings aren't ours to fix) |
| `agent_core` | static lib | all of `agent/**/*.c`; links `k3` + `agent_cjson` + `agent_sqlite`; compiled with `-Wall -Wextra -Wshadow -Wvla -Wno-unused-parameter` |
| `k3_agent` | executable | `agent/main.c`; output binary name **`k3-agent`** (via `set_target_properties(... OUTPUT_NAME k3-agent)`) |
| `agent_test_util` .. `agent_test_k3_engine` | executables + ctest entries | 11 test binaries (see below) |

`k3` itself (the model library) and `k3_cli` (output name `k3`) are untouched by
this file — the agent build is additive. No files under `src/`, `include/`,
`third_party/tok*.h`, `third_party/json.h`, `tests/unit/`, `cmake/`, `benchmarks/`,
or `tools/` are modified by the agent work.

## Running tests

```sh
ctest --test-dir build --output-on-failure
```

This runs every registered test, both the pre-existing K3 unit tests
(`test_ops`, `test_cache`, `test_st`, `test_cfg`, `test_tok`, `scale_test`,
`k3_model`, etc.) and the 11 agent tests:

- `agent_test_util` — buffer/log/jsonx utility coverage
- `agent_test_protocol` — the strict action-JSON grammar (extract/repair/validate)
- `agent_test_tools` — built-in tool behavior and argument validation
- `agent_test_security` — permissions, autonomy levels, sandbox path escapes
- `agent_test_planner` — goal → Plan generation and fallback synthesis
- `agent_test_task` — `AgentTask` lifecycle
- `agent_test_memory` — short-term ring, relevance retrieval, SQLite persistence
- `agent_test_loop` — the state machine and `agent_loop_run`/`agent_loop_resume`
- `agent_test_recovery` — `RecoveryClass` → `RecoveryStrategy` selection
- `agent_test_e2e` — end-to-end task runs against the mock backend
- `agent_test_k3_engine` — gates `k3_engine.c`'s forward path against the CLI oracle (opt-in; see below)

Every `agent_add_test` entry is invoked with the fixtures directory
`${CMAKE_CURRENT_SOURCE_DIR}/tests/agent/data` as `argv[1]`.

Combined with the pre-existing K3 suite this is the "21 tests" referenced
elsewhere in this doc set (10 K3 unit/oracle tests + 11 agent tests), all of which
must keep passing at every phase of agent development per the integration plan in
`docs/agent/analysis.md`.

## The mock backend: deterministic agent testing

`agent/model/mock_backend.c` implements the `ModelBackend` vtable without loading
any real model:

- `mock_backend_push(b, response)` queues a raw model-text response, served in
  FIFO order on successive `generate()` calls.
- `mock_backend_load_script(b, path)` loads a JSON array of strings from disk (used
  by `--backend mock --mock-script FILE`; see `docs/agent/examples.md` for a sample
  script).
- When the queue is exhausted, the backend synthesizes a `final` action JSON so a
  test or demo that runs past its scripted responses terminates deterministically
  instead of looping forever.
- `mock_backend_calls(b)` and `mock_backend_last_prompt(b)` let tests assert how
  many times the model was invoked and inspect the exact prompt the context
  builder produced — this is how `agent_test_loop`/`agent_test_e2e` verify context
  assembly without needing a real model.

Every agent test (`agent_test_loop`, `agent_test_e2e`, `agent_test_planner`, etc.)
builds an `Agent` around `mock_backend_create()` rather than `k3_backend_create()`,
so the full test suite runs in milliseconds with zero checkpoint dependency.

## Running the k3_engine gate against a real checkpoint

`agent_test_k3_engine` (`tests/agent/test_k3_engine.c`) is opt-in: it checks the
`K3_TINY_CKPT` environment variable at startup and prints `SKIPPED` (exit 0,
non-blocking) if it's unset. To actually run the gate:

```sh
python3 tools/make_tiny_checkpoint.py /tmp/tiny_ckpt
K3_TINY_CKPT=/tmp/tiny_ckpt ctest --test-dir build -R agent_test_k3_engine --output-on-failure
```

The test decodes 6 tokens greedily from a fixed 4-id prompt through
`k3_engine_generate_ids()` and compares against ids the upstream CLI produces for
the same prompt on the same tiny checkpoint. Any divergence is a hard failure —
this is what keeps `agent/model/k3_engine.c`'s independently-implemented forward
pass byte-identical in behavior to the oracle-validated `src/cli/k3_run.c` path
over time.

## Adding a new tool

1. Add a new source file under `agent/tools/` (or extend an existing one, e.g.
   `code.c` for coding helpers) and add it to the `add_library(agent_core STATIC ...)`
   source list in `agent/agent.cmake`.
2. Write the `execute()` function with signature
   `int (*)(const AgentTool *self, const cJSON *args, ToolContext *ctx, ToolResult *res)`.
   Use `jx_str`/`jx_int`/`jx_bool` (`agent/util/jsonx.h`) to read arguments, and
   `tool_result_ok`/`tool_result_okf`/`tool_result_fail` (`agent/tools/tool.h`) to
   fill the result. Route every filesystem path through `sandbox_resolve()`.
3. Add a registration function (pattern: `void my_tools_register(ToolRegistry *r)`)
   that builds an `AgentTool` struct per tool — set `name`, `description`,
   `args_schema` (shown verbatim to the model), `security_level`, and
   `requires_confirmation` if it should always gate — and calls
   `tool_registry_register(r, &t)`.
4. Call the new registration function from
   `tool_registry_register_builtins()` (`agent/tools/tool_registry.c`), alongside
   the existing `fs_tools_register`/`shell_tool_register`/etc. calls.
5. Add a test in `tests/agent/test_tools.c` (or a new test file wired into
   `agent/agent.cmake` via `agent_add_test`) covering success, validation-error,
   and (if relevant) sandbox-escape/permission-denied cases.
6. If the tool should have a non-default confirmation policy, add an entry to
   `config/permissions.json` and a security-level reference row to
   `config/tools.json`.

No other file needs to change — the registry, the protocol validator, and the
context builder's `AVAILABLE TOOLS` block all discover new tools automatically
through `tool_registry_describe()` and the dynamic tool-name list built each
iteration in `agent_loop.c`.
