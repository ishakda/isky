-- SPDX-License-Identifier: Apache-2.0
-- Agent persistence schema (build prompt §14). Applied idempotently at startup.

CREATE TABLE IF NOT EXISTS sessions (
    id          TEXT PRIMARY KEY,
    created_at  INTEGER NOT NULL,
    label       TEXT
);

CREATE TABLE IF NOT EXISTS tasks (
    id            TEXT PRIMARY KEY,
    goal          TEXT NOT NULL,
    state         TEXT NOT NULL,
    plan          TEXT,
    current_step  INTEGER DEFAULT 0,
    total_steps   INTEGER DEFAULT 0,
    iteration     INTEGER DEFAULT 0,
    max_iterations INTEGER DEFAULT 30,
    success       INTEGER DEFAULT 0,
    requires_user_input INTEGER DEFAULT 0,
    user_question TEXT,
    final_answer  TEXT,
    session_id    TEXT,
    created_at    INTEGER NOT NULL,
    updated_at    INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS task_steps (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id     TEXT NOT NULL,
    step_index  INTEGER NOT NULL,
    description TEXT,
    tool        TEXT,
    status      TEXT,               -- pending|running|done|failed
    created_at  INTEGER NOT NULL,
    FOREIGN KEY(task_id) REFERENCES tasks(id)
);

CREATE TABLE IF NOT EXISTS messages (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id     TEXT NOT NULL,
    role        TEXT NOT NULL,       -- system|user|assistant|tool
    content     TEXT,
    created_at  INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS tool_calls (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id     TEXT NOT NULL,
    step_index  INTEGER,
    tool        TEXT NOT NULL,
    arguments   TEXT,
    ok          INTEGER,
    error_class TEXT,
    created_at  INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS observations (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id     TEXT NOT NULL,
    step_index  INTEGER,
    content     TEXT,
    created_at  INTEGER NOT NULL
);

-- Unified memory store: type in {short_term, episodic, semantic, procedural}.
CREATE TABLE IF NOT EXISTS memories (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    type         TEXT NOT NULL,
    content      TEXT NOT NULL,
    importance   REAL DEFAULT 0.5,
    source_task  TEXT,
    created_at   INTEGER NOT NULL,
    last_used_at INTEGER
);

CREATE TABLE IF NOT EXISTS procedures (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    name        TEXT UNIQUE,
    steps       TEXT NOT NULL,
    source_task TEXT,
    created_at  INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS files (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id     TEXT,
    path        TEXT NOT NULL,
    action      TEXT,               -- read|write|edit|delete
    created_at  INTEGER NOT NULL
);

-- Execution trace (build prompt §34): ordered events per task.
CREATE TABLE IF NOT EXISTS trace (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id     TEXT NOT NULL,
    step_index  INTEGER,
    kind        TEXT NOT NULL,      -- event type name
    detail      TEXT,
    created_at  INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_mem_type ON memories(type);
CREATE INDEX IF NOT EXISTS idx_obs_task ON observations(task_id);
CREATE INDEX IF NOT EXISTS idx_trace_task ON trace(task_id);
CREATE INDEX IF NOT EXISTS idx_steps_task ON task_steps(task_id);
