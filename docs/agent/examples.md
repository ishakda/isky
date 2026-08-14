# Examples

All commands assume a build at `build/k3_agent` (`cmake -B build && cmake --build build`).

## Interactive REPL

Running with no goal-related flags drops into `repl()`:

```sh
build/k3_agent
```

```
> /help
Commands: /status /tasks /memory /tools /plan /permissions /model /config /reset /exit
> /tools
(tool_registry_describe output: name, description, args_schema per tool)
> /permissions
approval=risky autonomy=3
> /model
backend=k3 context_window=... max_tokens=1024 temperature=0.20
> Summarize the README in three bullets
(runs a full task to completion, printing EV_* progress lines as it goes)
> /exit
```

Any line not starting with `/` is treated as a new goal and run to completion via
`run_one()`. `/status`, `/plan`, and `/reset` are placeholders in the REPL — each
just prints `"(no active task in REPL; each goal runs to completion)"`, since the
REPL doesn't keep a task alive across turns (each line is one self-contained run).

## One-shot task

```sh
build/k3_agent --task "List all TODO comments in agent/"
```

Runs a single task to a terminal outcome (or `WAITING_USER`, in which case
`run_one()` interactively reads an answer from stdin and calls
`agent_loop_resume()` in a loop until the task terminates).

Equivalently, from a file:

```sh
build/k3_agent --task-file goal.txt
```

## Plan-only mode

```sh
build/k3_agent --plan "Refactor the tokenizer wrapper for clarity"
```

Sets `plan_only = 1`: the loop generates a `Plan` via `planner_make_plan()` and
stops — no tool calls, no execution. Useful for inspecting what the model would
attempt before authorizing it to run.

## Dry-run mode

```sh
build/k3_agent --dry-run "Delete all .tmp files in the workspace"
```

Sets `dry_run = 1`: the loop still plans and calls `decide_action` each iteration,
but tool actions are reported (and logged) as what *would* run without actually
calling `tool_registry_execute`. Useful for previewing a risky task's shape.

## Review mode

```sh
build/k3_agent review /path/to/some/project
```

Runs a task whose goal is the fixed string:

> "Review the project at *DIR*. Inspect architecture, look for bugs, security
> issues, performance and maintainability concerns, and report structured
> findings graded CRITICAL/HIGH/MEDIUM/LOW/INFO."

with the sandbox workspace unaffected — this only sets the goal text; you still
need `--workspace` pointed at (or containing) the target project if it isn't
already the working directory, since filesystem tools stay confined to the
sandbox root.

## Resuming a paused task

If a task returned `RUN_NEEDS_USER` (e.g. from a non-interactive invocation, or a
prior REPL/API session), and a database is attached (`memory.enabled: true`,
which is the default), resume it by task id:

```sh
build/k3_agent --resume 3f9c2a11-...
```

`main.c` requires `a->db` to be non-NULL for `--resume` (errors "resume needs the
database enabled" otherwise), loads the task via `db_load_task`, prompts on stdin
for the answer to `task.user_question`, and calls `agent_loop_resume()`.

## Deterministic demos with the mock backend

```sh
build/k3_agent --backend mock --mock-script hello.json --task "Write hello.txt and read it back"
```

`hello.json` — a JSON array of raw model-text responses served in order, one per
`generate()` call. A hello-world flow (`plan` → `write_file` → `read_file` →
`final`):

```json
[
  "{\"action\":\"plan\",\"steps\":[{\"id\":1,\"description\":\"Write hello.txt\",\"tool\":\"write_file\"},{\"id\":2,\"description\":\"Read hello.txt back\",\"tool\":\"read_file\"}]}",
  "{\"action\":\"tool\",\"tool\":\"write_file\",\"arguments\":{\"path\":\"hello.txt\",\"content\":\"Hello, agent!\",\"mode\":\"overwrite\"},\"expected_result\":\"file written\"}",
  "{\"action\":\"tool\",\"tool\":\"read_file\",\"arguments\":{\"path\":\"hello.txt\"},\"expected_result\":\"file contents returned\"}",
  "{\"action\":\"final\",\"answer\":\"Wrote hello.txt and confirmed its contents: Hello, agent!\"}"
]
```

Each array element is exactly the raw text a real model turn would need to
produce — the mock backend hands it straight to `agent_action_parse()`, so it must
be valid action JSON. Because responses are consumed strictly in order and the
queue synthesizes an automatic `final` once exhausted, this script always
terminates deterministically — the basis for `agent_test_e2e` and any
reproducible demo/CI run that shouldn't depend on real model output.

The same effect without a script file, useful in ad hoc test code, is
`mock_backend_push(b, response)` called once per expected turn before
`agent_loop_run()`.

## Real-model invocation sketch

```sh
build/k3_agent --backend k3 --task "Summarize agent/README notes" --workspace ./workspace
```

`main.c` has no direct `--model-dir`/`--tok-dir` flags — the K3 backend is
configured entirely through `config/agent.json`'s `model.*` fields (`model_dir`,
`tok_dir`, `config_path`, `trunk_dir`, `trunk_gb`, `cache_gb`, `layers`,
`max_tokens`, `temperature`, `top_p`), which `make_backend()` reads into a
`K3EngineOpts` before calling `k3_backend_create()`. To point at a specific
checkpoint, edit (or `--config` to a copy of) `config/agent.json`:

```json
{
  "model": {
    "backend": "k3",
    "model_dir": "/path/to/checkpoint",
    "tok_dir": "/path/to/checkpoint",
    "cache_gb": 2.0,
    "max_tokens": 1024,
    "temperature": 0.2
  }
}
```

then:

```sh
build/k3_agent --config my_agent.json --task "Summarize agent/README notes"
```

`--backend k3` on the CLI is only needed if the config file has `model.backend`
set to `"mock"` and you want to override it back to the real engine for that run.
