# Configuration

Configuration is loaded by `agent_config_load()` (`agent/core/config.c`) from a
JSON file (default `config/agent.json`, override with `--config`). Every field has
a hardcoded default in `agent_config_defaults()`, applied **before** the file is
read — a missing file is not an error (falls back to defaults, returns 0); a
malformed file **is** an error (returns -1 and the CLI aborts). Nothing in the
runtime hardcodes a value outside this loader; every call site reads from
`AgentConfig`.

## `config/agent.json` fields

| JSON path | Field (`AgentConfig`) | Default | Notes |
|---|---|---|---|
| `model.backend` | `backend` | `"k3"` | `"k3"` or `"mock"` |
| `model.model_dir` | `model_dir` | `"./model"` | checkpoint directory |
| `model.tok_dir` | `tok_dir` | `""` | tokenizer dir; empty = id-only sessions |
| `model.config_path` | `cfg_path` | `""` | explicit `config.json`; empty = use model_dir's |
| `model.trunk_dir` | `trunk_dir` | `""` | packed trunk dir for streamed (non-resident) weights |
| `model.trunk_gb` | `trunk_gb` | `0` | streaming byte budget when `trunk_dir` set |
| `model.cache_gb` | `cache_gb` | `0.5` | expert cache budget |
| `model.layers` | `n_layers` | `0` | `0` = all layers |
| `model.max_tokens` | `max_tokens` | `1024` | max tokens per model call |
| `model.temperature` | `temperature` | `0.2` | `0` = greedy |
| `model.top_p` | `top_p` | `0.95` | `1.0` disables nucleus sampling |
| `agent.max_iterations` | `max_iterations` | `30` | loop iteration cap per task |
| `agent.max_retries` | `max_retries` | `3` | retries before a recovery escalates |
| `agent.max_reflections` | `max_reflections` | `2` | `ACT_REFLECT` budget per step |
| `agent.max_repair_attempts` | `max_repair_attempts` | `5` | coding-loop repair attempts (see note below) |
| `agent.autonomy_level` | `autonomy_level` | `3` | `0`-`4`, clamped after load |
| `memory.enabled` | `memory_enabled` | `1` (true) | disables SQLite persistence entirely when false |
| `memory.database` | `database` | `"agent.db"` | SQLite path (or `:memory:`) |
| `security.sandbox` | `sandbox` | `1` (true) | disabling removes path-escape checking |
| `security.workspace` | `workspace` | `"./workspace"` | sandbox root |
| `security.confirm_dangerous_tools` | `confirm_dangerous_tools` | `1` (true) | present in config but the actual confirmation logic lives in `permissions_needs_confirmation`/`AgentTool.requires_confirmation`, not a direct read of this flag |
| `security.approval` | `approval` | `"risky"` | `"always"` \| `"risky"` \| `"never"` |
| `prompts.system` | `system_prompt_path` | `AGENT_PROMPT_DIR "/system.txt"` (build-time define, effectively `agent/prompts/system.txt`) | |
| `logging.level` | `log_level` | `"INFO"` | `TRACE`\|`DEBUG`\|`INFO`\|`WARN`\|`ERROR` |
| `logging.file` | `log_file` | `""` | empty = stderr only |
| `api.enabled` | `api_enabled` | `0` (false) | present in config; the CLI's `--serve` flag is what actually starts the server (see below) |
| `api.port` | `api_port` | `8080` | |

`max_repair_attempts` bounds the compile/test fix loop. When a step fails with a
`COMPILATION_ERROR` or `TEST_FAILURE`, `recovery_strategy_for2()` returns
`STRAT_REPLAN` (let the model try a different edit) until the attempt count reaches
`max_repair_attempts`, at which point it returns `STRAT_ABORT` rather than looping
forever. Transient failures (timeout, network, tool) are bounded separately by
`max_retries`.

## `config/permissions.json`

Loaded by `permissions_load()` (`agent/security/permissions.c`), applied on top of
whatever `Permissions` object `agent_create()` built from `AgentConfig`
(`approval`, `autonomy_level`). Top-level `approval` / `autonomy_level` keys
override the agent-config values; everything under `tools` sets a per-tool
`{ enabled, require_confirmation }` override:

```json
{
  "approval": "risky",
  "autonomy_level": 3,
  "tools": {
    "shell_execute": { "enabled": true, "require_confirmation": true },
    "delete_file":   { "enabled": true, "require_confirmation": true }
  }
}
```

A tool with no explicit entry defaults to `enabled: true` in
`permissions_is_enabled()`. See the full shipped defaults table in
`docs/agent/tools.md` and `docs/agent/security.md` for how `enabled` and
`require_confirmation` combine with security level and autonomy.

## `config/tools.json`

Documentation-only reference mapping each built-in tool name to its intrinsic
`security_level` string. This file is **not read by the runtime** — the
authoritative security level is the one compiled into each tool's `AgentTool`
registration in `agent/tools/*.c`. It exists so a human/operator can see the
security model at a glance without reading C source.

## CLI overrides (`agent/main.c`)

Parsed before `agent_config_load()` is not quite right — the config is loaded
first, then these flags overlay it:

| Flag | Overrides |
|---|---|
| `--config F` | config file path itself (default `config/agent.json`) |
| `--backend k3\|mock` | `cfg.backend` |
| `--approval always\|risky\|never` | `cfg.approval` (via `agent_config_parse_approval`) |
| `--autonomy 0..4` | `cfg.autonomy_level` |
| `--workspace DIR` | `cfg.workspace` |
| `--log LEVEL` | `cfg.log_level` |
| `--port N` | port used by `--serve` (falls back to `cfg.api_port` if `--port` omitted) |

Flags not tied to a config field: `--task "..."`, `--task-file F`, `--resume ID`,
`--plan "..."` (implies plan-only), `--dry-run "..."` (implies dry-run execution),
`review DIR`, `--mock-script F` (only meaningful with `--backend mock`),
`--serve`. There is no environment-variable override path in `main.c` itself for
`AgentConfig` fields — the only environment variable in the agent+model stack is
`K3_TINY_CKPT`, consumed by the `test_k3_engine` gate (see
`docs/agent/development.md`), not by the agent runtime's normal config loading.
