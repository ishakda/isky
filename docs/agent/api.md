# HTTP API

`agent/api/server.c` implements a minimal, synchronous, **loopback-only** HTTP
server. It is POSIX-only — on `_WIN32` the entire module is stubbed
(`api_server_create` returns `NULL`, `api_server_run` returns `-1`); there is no
Windows API server today.

## Starting it

```sh
build/k3_agent --serve --port 8080
```

(`--port` falls back to `config/agent.json`'s `api.port`, default `8080`, if
omitted.) `api_server_create()` binds to `INADDR_LOOPBACK` (`127.0.0.1`)
exclusively — it never listens on `0.0.0.0` or any other interface, so it is not
reachable off the local machine. `api_server_run()` is a single-threaded, blocking
accept loop: one connection at a time, `Connection: close` on every response (no
keep-alive), request bodies read up to 65536 bytes.

## Endpoints

The server implements **exactly four** routes. Anything else returns `404`.

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/v1/tools` | List every registered tool |
| `GET` | `/v1/memory` | List recent persisted memories (any type) |
| `POST` | `/v1/tasks` | Create and **synchronously run** a task to completion/pause/failure |
| `GET` | `/v1/tasks/:id` | Fetch a task's final state, answer, and full trace |
| `POST` | `/v1/tasks/:id/cancel` | Mark a persisted task cancelled (FAILED) |
| `POST` | `/v1/tasks/:id/resume` | Resume a `waiting_user` task with `{"answer":"..."}` |
| `GET` | `/v1/tasks/:id/events` | Fetch the persisted execution trace (post-hoc snapshot) |

### `GET /v1/tools`

Response: JSON array, one entry per tool in the registry.

```json
[
  { "name": "read_file", "description": "...", "security_level": "SAFE" },
  { "name": "shell_execute", "description": "...", "security_level": "MEDIUM_RISK" }
]
```

### `GET /v1/memory`

Response: JSON array from `db_search_memories(db, NULL, NULL, 50)` — up to 50
memories across all types, unfiltered by query, most-recently-used first. Returns
`[]` if no database is attached (`memory.enabled: false`).

### `POST /v1/tasks`

Request body:

```json
{ "goal": "Summarize README.md" }
```

Missing/empty `goal` → `400 { "error": "goal required" }`.

Otherwise the server creates a task (`task_create(goal, cfg.max_iterations)`) and
calls `agent_loop_run()` **synchronously** — the HTTP request blocks until the
task reaches a terminal or paused state. There is no polling/queuing model; the
connection is held open for the task's full duration.

Response:

```json
{
  "task_id": "…",
  "status": "completed" | "waiting_user" | "failed",
  "answer": "…",       // present when status == "completed"
  "question": "…"      // present when status == "waiting_user"
}
```

### `GET /v1/tasks/:id`

`404 { "error": "not found" }` if there is no database attached or the id doesn't
exist. Otherwise:

```json
{
  "task_id": "…",
  "state": "COMPLETED",
  "success": true,
  "answer": "…",
  "trace": [ { "step_index": 0, "kind": "EV_PLAN_CREATED", "detail": "…" }, "…" ]
}
```

`trace` is the ordered `trace` table for that task (`db_get_trace`) — every event
the loop emitted, in order.

## Additional endpoints

- **`POST /v1/tasks/:id/cancel`** — marks a persisted task `FAILED` with a
  cancellation note and saves it. Because `POST /v1/tasks` runs synchronously on a
  single-threaded server, this cancels a *persisted* task between runs rather than
  interrupting one mid-flight.
- **`POST /v1/tasks/:id/resume`** — resumes a `waiting_user` task, optionally with
  a JSON body `{"answer":"..."}`; calls `agent_loop_resume()` and returns the new
  status/answer. The same operation is available on the CLI via
  `k3-agent --resume TASK_ID`.
- **`GET /v1/tasks/:id/events`** — returns the persisted execution **trace** as a
  JSON array: the same event history the CLI's `on_event()` handler prints live
  (`EV_PLAN_CREATED`, `EV_STEP_STARTED`, `EV_TOOL_SELECTED`, `EV_TOOL_FAILED`,
  `EV_VERIFICATION_PASSED/FAILED`, `EV_RECOVERY_STARTED`, `EV_USER_INPUT_REQUIRED`,
  …), delivered as a post-hoc snapshot rather than a streaming SSE feed. For live,
  incremental progress, drive the agent through the CLI, whose event handler prints
  each event as it happens.

The server is intentionally minimal (loopback-only, single-threaded) — a local
control plane, not a public API surface.
