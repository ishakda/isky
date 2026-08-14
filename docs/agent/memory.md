# Memory

`agent/memory/memory.h` implements four memory types. Not every message is stored
— the loop selectively records episodes, facts, and procedures at meaningful
points, and short-term memory is bounded and RAM-only.

| Type | Storage | Lifetime | Written by |
|---|---|---|---|
| **short_term** | RAM (`ShortTerm` ring buffer in `Memory.st`) | one task | `memory_add_observation()` after every tool result |
| **episodic** | SQLite `memories` table, `type='episodic'` | persistent | `memory_record_episode()` on tool failure (records task/action/result/solution as a JSON blob) |
| **semantic** | SQLite `memories` table, `type='semantic'` | persistent | `memory_record_fact()` (available to callers; not currently invoked by the core loop itself) |
| **procedural** | SQLite `procedures` table (+ mirrored into `memories` conceptually via `type='procedural'` lookups) | persistent | `memory_record_procedure()` on task success — stores the finished plan JSON keyed `"goal:<goal text>"` |

## Short-term memory

`ShortTerm` (`agent/memory/memory.h`) is a fixed-capacity ring (`cap` set to 16 in
`agent_loop.c`'s `loop_setup_and_run`) holding the task's `goal`, current `plan`
JSON, and a ring of recent observation strings. `memory_add_observation()` pushes
new observations, evicting the oldest once full; each addition is also logged to
`db_log_observation` if a database is attached (persisted for the trace, but not
part of the retrieval/relevance system). `memory_recent_observations(max_chars)`
joins the ring oldest-to-newest and trims from the front (keeping the most recent
tail) if the result exceeds `max_chars` — this is what feeds the `OBSERVATIONS`
section of the model prompt (default budget 4000 chars, from `decide_action`).

## Persistent memory and the relevance formula

`db_search_memories(db, type, query, limit)` in `agent/storage/database.c` is the
retrieval function behind `memory_recall()`. It pulls up to 200 candidate rows of
the requested type (or any type) ordered by `last_used_at DESC`, then scores each
candidate in C:

```c
double age_days = (now - created) / 86400.0;
double recency  = 1.0 / (1.0 + age_days);
double overlap  = word_overlap(content, query);   /* count of query words (len>=3) present as case-insensitive substrings */
score = (overlap + 0.1) * (0.5 + importance) * (0.5 + recency);
```

This is **overlap × importance × recency**, each additively floored so a memory
with zero keyword overlap or zero recorded importance still has a nonzero (small)
score rather than being excluded outright. A selection sort then picks the top
`limit` by score. `memory_recall(query, max_items)` calls this three times — once
each for `semantic`, `episodic`, `procedural` — and assembles a labeled text block
(`Known facts:` / `Past experiences:` / `Procedures:`), each bullet being one
memory's `content` field. This block is what `decide_action` requests via
`memory_recall(goal, 6)` and hands to `context_build` as the `MEMORY` section.

## SQLite schema

Applied idempotently at startup from `agent/storage/schema.sql`:

| Table | Purpose |
|---|---|
| `sessions` | id, created_at, label — a grouping concept for tasks (minimal use in the current implementation) |
| `tasks` | Full `AgentTask` snapshot: goal, state, plan, current_step, total_steps, iteration, max_iterations, success, requires_user_input, user_question, final_answer, session_id, timestamps. Enables crash-safe resume (`db_save_task` / `db_load_task`). |
| `task_steps` | Per-step log: task_id, step_index, description, tool, status (`pending`\|`running`\|`done`\|`failed`) |
| `messages` | Raw model/user turns: role (`system`\|`user`\|`assistant`\|`tool`), content — used for `db_log_message` calls in the loop |
| `tool_calls` | Every tool invocation: task_id, step_index, tool, arguments (JSON text), ok, error_class |
| `observations` | Every short-term observation, also persisted here for the trace |
| `memories` | The unified store for `episodic`/`semantic`/`procedural` (and conceptually `short_term`, though short-term never actually reaches this table): type, content, importance, source_task, created_at, last_used_at |
| `procedures` | Named reusable plans: `name` (unique), `steps` (JSON), source_task |
| `files` | File-touch audit log: task_id, path, action (`read`\|`write`\|`edit`\|`delete`) |
| `trace` | Ordered per-task event log: step_index, kind (event type name), detail — this is what `GET /v1/tasks/:id` returns as `"trace"` |

Indexes: `idx_mem_type` (memories.type), `idx_obs_task` (observations.task_id),
`idx_trace_task` (trace.task_id), `idx_steps_task` (task_steps.task_id).

`db_open(path)` accepts `":memory:"` for ephemeral/test databases. Memory can be
disabled entirely via `config/agent.json`'s `"memory.enabled": false`, in which
case `Agent.db` is NULL and `Memory` degrades gracefully to RAM-only short-term
tracking (every `db`-guarded call in `memory.c` becomes a no-op).

## Context builder budgeting and compression

`context_build()` (`agent/context/context.c`) assembles the full model prompt from
labeled sections in a fixed order: system prompt → `ROLE` → `TASK` (goal) →
`CURRENT STATE` → `CURRENT STEP` → `PLAN` → `AVAILABLE TOOLS` → `MEMORY` →
`OBSERVATIONS` → `NOTE` (recovery/reflection hint) → the mandatory
`REQUIRED OUTPUT FORMAT` block (`context_output_format()`, which spells out the
five JSON action shapes verbatim).

It counts tokens via `backend->count_tokens` (falling back to `bytes/4` when the
backend has no tokenizer loaded) and compares against `budget_tokens`
(`backend->context_window`). If the first assembly is under budget, it returns
immediately. Otherwise it runs up to two more compression passes: it computes how
many characters need to be cut, then applies `tail_trim()` — which keeps the
**tail** (most recent content, cut at the next newline boundary) and prepends
`"[... earlier context compressed ...]"` — first to `OBSERVATIONS`, then to
`MEMORY` if more cutting is still needed. The system prompt, goal, and plan are
never truncated; if the budget still can't be met after two compression passes, a
warning is logged and the (still slightly oversized) prompt is sent anyway rather
than silently dropping the task-defining sections.
