/* SPDX-License-Identifier: Apache-2.0 */
#include "database.h"
#include "../util/log.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sqlite3.h"

/* Schema embedded so the binary has no runtime file dependency. Kept in sync with
 * agent/storage/schema.sql (that file is the documented reference copy). */
static const char *SCHEMA_SQL =
"CREATE TABLE IF NOT EXISTS sessions(id TEXT PRIMARY KEY,created_at INTEGER NOT NULL,label TEXT);"
"CREATE TABLE IF NOT EXISTS tasks(id TEXT PRIMARY KEY,goal TEXT NOT NULL,state TEXT NOT NULL,"
"plan TEXT,current_step INTEGER DEFAULT 0,total_steps INTEGER DEFAULT 0,iteration INTEGER DEFAULT 0,"
"max_iterations INTEGER DEFAULT 30,success INTEGER DEFAULT 0,requires_user_input INTEGER DEFAULT 0,"
"user_question TEXT,final_answer TEXT,session_id TEXT,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL);"
"CREATE TABLE IF NOT EXISTS task_steps(id INTEGER PRIMARY KEY AUTOINCREMENT,task_id TEXT NOT NULL,"
"step_index INTEGER NOT NULL,description TEXT,tool TEXT,status TEXT,created_at INTEGER NOT NULL);"
"CREATE TABLE IF NOT EXISTS messages(id INTEGER PRIMARY KEY AUTOINCREMENT,task_id TEXT NOT NULL,"
"role TEXT NOT NULL,content TEXT,created_at INTEGER NOT NULL);"
"CREATE TABLE IF NOT EXISTS tool_calls(id INTEGER PRIMARY KEY AUTOINCREMENT,task_id TEXT NOT NULL,"
"step_index INTEGER,tool TEXT NOT NULL,arguments TEXT,ok INTEGER,error_class TEXT,created_at INTEGER NOT NULL);"
"CREATE TABLE IF NOT EXISTS observations(id INTEGER PRIMARY KEY AUTOINCREMENT,task_id TEXT NOT NULL,"
"step_index INTEGER,content TEXT,created_at INTEGER NOT NULL);"
"CREATE TABLE IF NOT EXISTS memories(id INTEGER PRIMARY KEY AUTOINCREMENT,type TEXT NOT NULL,"
"content TEXT NOT NULL,importance REAL DEFAULT 0.5,source_task TEXT,created_at INTEGER NOT NULL,last_used_at INTEGER);"
"CREATE TABLE IF NOT EXISTS procedures(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT UNIQUE,"
"steps TEXT NOT NULL,source_task TEXT,created_at INTEGER NOT NULL);"
"CREATE TABLE IF NOT EXISTS files(id INTEGER PRIMARY KEY AUTOINCREMENT,task_id TEXT,path TEXT NOT NULL,"
"action TEXT,created_at INTEGER NOT NULL);"
"CREATE TABLE IF NOT EXISTS trace(id INTEGER PRIMARY KEY AUTOINCREMENT,task_id TEXT NOT NULL,"
"step_index INTEGER,kind TEXT NOT NULL,detail TEXT,created_at INTEGER NOT NULL);"
"CREATE INDEX IF NOT EXISTS idx_mem_type ON memories(type);"
"CREATE INDEX IF NOT EXISTS idx_obs_task ON observations(task_id);"
"CREATE INDEX IF NOT EXISTS idx_trace_task ON trace(task_id);"
"CREATE INDEX IF NOT EXISTS idx_steps_task ON task_steps(task_id);";

struct Database { sqlite3 *db; };

Database *db_open(const char *path)
{
    Database *d = (Database *)calloc(1, sizeof *d);
    if (!d) return NULL;
    if (sqlite3_open(path, &d->db) != SQLITE_OK) {
        LOG_E("db", "cannot open %s: %s", path, sqlite3_errmsg(d->db));
        sqlite3_close(d->db);
        free(d);
        return NULL;
    }
    sqlite3_busy_timeout(d->db, 5000);
    sqlite3_exec(d->db, "PRAGMA journal_mode=WAL;PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    char *err = NULL;
    if (sqlite3_exec(d->db, SCHEMA_SQL, NULL, NULL, &err) != SQLITE_OK) {
        LOG_E("db", "schema failed: %s", err ? err : "?");
        sqlite3_free(err);
        sqlite3_close(d->db);
        free(d);
        return NULL;
    }
    return d;
}

void db_close(Database *d)
{
    if (!d) return;
    if (d->db) sqlite3_close(d->db);
    free(d);
}

static int bind_text(sqlite3_stmt *s, int i, const char *v)
{
    return sqlite3_bind_text(s, i, v ? v : NULL, -1, SQLITE_TRANSIENT);
}

int db_save_task(Database *d, const AgentTask *t)
{
    const char *sql =
        "INSERT INTO tasks(id,goal,state,plan,current_step,total_steps,iteration,"
        "max_iterations,success,requires_user_input,user_question,final_answer,"
        "created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET goal=excluded.goal,state=excluded.state,"
        "plan=excluded.plan,current_step=excluded.current_step,total_steps=excluded.total_steps,"
        "iteration=excluded.iteration,max_iterations=excluded.max_iterations,"
        "success=excluded.success,requires_user_input=excluded.requires_user_input,"
        "user_question=excluded.user_question,final_answer=excluded.final_answer,"
        "updated_at=excluded.updated_at;";
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(d->db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    bind_text(s, 1, t->id);
    bind_text(s, 2, t->goal);
    bind_text(s, 3, agent_state_name(t->state));
    bind_text(s, 4, t->plan);
    sqlite3_bind_int(s, 5, t->current_step);
    sqlite3_bind_int(s, 6, t->total_steps);
    sqlite3_bind_int(s, 7, t->iteration);
    sqlite3_bind_int(s, 8, t->max_iterations);
    sqlite3_bind_int(s, 9, t->success);
    sqlite3_bind_int(s, 10, t->requires_user_input);
    bind_text(s, 11, t->user_question);
    bind_text(s, 12, t->final_answer);
    sqlite3_bind_int64(s, 13, (sqlite3_int64)t->created_at);
    sqlite3_bind_int64(s, 14, (sqlite3_int64)t->updated_at);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

static AgentState state_from_name(const char *n)
{
    for (int i = AGENT_IDLE; i <= AGENT_WAITING_USER; i++)
        if (n && !strcmp(n, agent_state_name((AgentState)i))) return (AgentState)i;
    return AGENT_IDLE;
}

int db_load_task(Database *d, const char *task_id, AgentTask *t)
{
    const char *sql = "SELECT goal,state,plan,current_step,total_steps,iteration,"
        "max_iterations,success,requires_user_input,user_question,final_answer,"
        "created_at,updated_at FROM tasks WHERE id=?;";
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(d->db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    bind_text(s, 1, task_id);
    int rc = sqlite3_step(s);
    if (rc != SQLITE_ROW) { sqlite3_finalize(s); return rc == SQLITE_DONE ? 0 : -1; }

    snprintf(t->id, sizeof t->id, "%s", task_id);
    const char *g = (const char *)sqlite3_column_text(s, 0);
    t->goal = strdup(g ? g : "");
    t->state = state_from_name((const char *)sqlite3_column_text(s, 1));
    const char *pl = (const char *)sqlite3_column_text(s, 2);
    t->plan = pl ? strdup(pl) : NULL;
    t->current_step = sqlite3_column_int(s, 3);
    t->total_steps = sqlite3_column_int(s, 4);
    t->iteration = sqlite3_column_int(s, 5);
    t->max_iterations = sqlite3_column_int(s, 6);
    t->success = sqlite3_column_int(s, 7);
    t->requires_user_input = sqlite3_column_int(s, 8);
    const char *uq = (const char *)sqlite3_column_text(s, 9);
    t->user_question = uq ? strdup(uq) : NULL;
    const char *fa = (const char *)sqlite3_column_text(s, 10);
    t->final_answer = fa ? strdup(fa) : NULL;
    t->created_at = (time_t)sqlite3_column_int64(s, 11);
    t->updated_at = (time_t)sqlite3_column_int64(s, 12);
    sqlite3_finalize(s);
    return 1;
}

cJSON *db_list_tasks(Database *d, int limit)
{
    if (limit <= 0) limit = 50;
    const char *sql = "SELECT id,goal,state,success,updated_at FROM tasks "
                      "ORDER BY updated_at DESC LIMIT ?;";
    sqlite3_stmt *s = NULL;
    cJSON *arr = cJSON_CreateArray();
    if (sqlite3_prepare_v2(d->db, sql, -1, &s, NULL) != SQLITE_OK) return arr;
    sqlite3_bind_int(s, 1, limit);
    while (sqlite3_step(s) == SQLITE_ROW) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", (const char *)sqlite3_column_text(s, 0));
        cJSON_AddStringToObject(o, "goal", (const char *)sqlite3_column_text(s, 1));
        cJSON_AddStringToObject(o, "state", (const char *)sqlite3_column_text(s, 2));
        cJSON_AddNumberToObject(o, "success", sqlite3_column_int(s, 3));
        cJSON_AddNumberToObject(o, "updated_at", (double)sqlite3_column_int64(s, 4));
        cJSON_AddItemToArray(arr, o);
    }
    sqlite3_finalize(s);
    return arr;
}

static int exec_log(Database *d, const char *sql,
                    void (*bind)(sqlite3_stmt *, void *), void *ud)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(d->db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    bind(s, ud);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_log_step(Database *d, const char *task_id, int idx, const char *desc,
                const char *tool, const char *status)
{
    sqlite3_stmt *s = NULL;
    const char *sql = "INSERT INTO task_steps(task_id,step_index,description,tool,status,created_at)"
                      " VALUES(?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(d->db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    bind_text(s, 1, task_id); sqlite3_bind_int(s, 2, idx);
    bind_text(s, 3, desc); bind_text(s, 4, tool); bind_text(s, 5, status);
    sqlite3_bind_int64(s, 6, (sqlite3_int64)time(NULL));
    int rc = sqlite3_step(s); sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_log_tool_call(Database *d, const char *task_id, int idx, const char *tool,
                     const char *args_json, int ok, const char *error_class)
{
    sqlite3_stmt *s = NULL;
    const char *sql = "INSERT INTO tool_calls(task_id,step_index,tool,arguments,ok,error_class,created_at)"
                      " VALUES(?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(d->db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    bind_text(s, 1, task_id); sqlite3_bind_int(s, 2, idx); bind_text(s, 3, tool);
    bind_text(s, 4, args_json); sqlite3_bind_int(s, 5, ok); bind_text(s, 6, error_class);
    sqlite3_bind_int64(s, 7, (sqlite3_int64)time(NULL));
    int rc = sqlite3_step(s); sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_log_observation(Database *d, const char *task_id, int idx, const char *content)
{
    sqlite3_stmt *s = NULL;
    const char *sql = "INSERT INTO observations(task_id,step_index,content,created_at) VALUES(?,?,?,?);";
    if (sqlite3_prepare_v2(d->db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    bind_text(s, 1, task_id); sqlite3_bind_int(s, 2, idx); bind_text(s, 3, content);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)time(NULL));
    int rc = sqlite3_step(s); sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_log_message(Database *d, const char *task_id, const char *role, const char *content)
{
    sqlite3_stmt *s = NULL;
    const char *sql = "INSERT INTO messages(task_id,role,content,created_at) VALUES(?,?,?,?);";
    if (sqlite3_prepare_v2(d->db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    bind_text(s, 1, task_id); bind_text(s, 2, role); bind_text(s, 3, content);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)time(NULL));
    int rc = sqlite3_step(s); sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_log_trace(Database *d, const char *task_id, int idx, const char *kind, const char *detail)
{
    sqlite3_stmt *s = NULL;
    const char *sql = "INSERT INTO trace(task_id,step_index,kind,detail,created_at) VALUES(?,?,?,?,?);";
    if (sqlite3_prepare_v2(d->db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    bind_text(s, 1, task_id); sqlite3_bind_int(s, 2, idx); bind_text(s, 3, kind);
    bind_text(s, 4, detail); sqlite3_bind_int64(s, 5, (sqlite3_int64)time(NULL));
    int rc = sqlite3_step(s); sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_log_file(Database *d, const char *task_id, const char *path, const char *action)
{
    sqlite3_stmt *s = NULL;
    const char *sql = "INSERT INTO files(task_id,path,action,created_at) VALUES(?,?,?,?);";
    if (sqlite3_prepare_v2(d->db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    bind_text(s, 1, task_id); bind_text(s, 2, path); bind_text(s, 3, action);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)time(NULL));
    int rc = sqlite3_step(s); sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_add_memory(Database *d, const char *type, const char *content,
                  double importance, const char *source_task)
{
    sqlite3_stmt *s = NULL;
    const char *sql = "INSERT INTO memories(type,content,importance,source_task,created_at,last_used_at)"
                      " VALUES(?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(d->db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    bind_text(s, 1, type); bind_text(s, 2, content);
    sqlite3_bind_double(s, 3, importance); bind_text(s, 4, source_task);
    sqlite3_bind_int64(s, 5, (sqlite3_int64)time(NULL));
    sqlite3_bind_int64(s, 6, (sqlite3_int64)time(NULL));
    int rc = sqlite3_step(s); sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Relevance = keyword overlap, weighted by importance and recency. Done in C over a
 * bounded candidate set to keep it dependency-free. */
static int word_overlap(const char *text, const char *query)
{
    if (!query || !query[0]) return 0;
    int score = 0;
    char q[512];
    snprintf(q, sizeof q, "%s", query);
    for (char *tok = strtok(q, " \t\n,.;:"); tok; tok = strtok(NULL, " \t\n,.;:")) {
        if (strlen(tok) < 3) continue;
        /* case-insensitive substring */
        size_t tl = strlen(tok);
        for (const char *p = text; *p; p++)
            if (strncasecmp(p, tok, tl) == 0) { score++; break; }
    }
    return score;
}

cJSON *db_search_memories(Database *d, const char *type, const char *query, int limit)
{
    if (limit <= 0) limit = 8;
    char sql[256];
    if (type)
        snprintf(sql, sizeof sql, "SELECT id,type,content,importance,created_at FROM memories "
                 "WHERE type=? ORDER BY last_used_at DESC LIMIT 200;");
    else
        snprintf(sql, sizeof sql, "SELECT id,type,content,importance,created_at FROM memories "
                 "ORDER BY last_used_at DESC LIMIT 200;");
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(d->db, sql, -1, &s, NULL) != SQLITE_OK) return cJSON_CreateArray();
    if (type) bind_text(s, 1, type);

    /* collect + score */
    typedef struct { int id; char *type; char *content; double imp; double score; } Cand;
    Cand *c = NULL; int n = 0, cap = 0;
    time_t now = time(NULL);
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (n == cap) { cap = cap ? cap * 2 : 32; c = realloc(c, (size_t)cap * sizeof *c); }
        c[n].id = sqlite3_column_int(s, 0);
        c[n].type = strdup((const char *)sqlite3_column_text(s, 1));
        c[n].content = strdup((const char *)sqlite3_column_text(s, 2));
        c[n].imp = sqlite3_column_double(s, 3);
        time_t created = (time_t)sqlite3_column_int64(s, 4);
        double age_days = (double)(now - created) / 86400.0;
        double recency = 1.0 / (1.0 + age_days);
        double overlap = query ? (double)word_overlap(c[n].content, query) : 0.0;
        c[n].score = (overlap + 0.1) * (0.5 + c[n].imp) * (0.5 + recency);
        n++;
    }
    sqlite3_finalize(s);

    /* selection sort top `limit` */
    cJSON *arr = cJSON_CreateArray();
    for (int k = 0; k < limit && k < n; k++) {
        int best = -1;
        for (int i = 0; i < n; i++)
            if (c[i].content && (best < 0 || c[i].score > c[best].score)) best = i;
        if (best < 0) break;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", c[best].id);
        cJSON_AddStringToObject(o, "type", c[best].type);
        cJSON_AddStringToObject(o, "content", c[best].content);
        cJSON_AddNumberToObject(o, "importance", c[best].imp);
        cJSON_AddItemToArray(arr, o);
        free(c[best].content); c[best].content = NULL;   /* mark consumed */
    }
    for (int i = 0; i < n; i++) { free(c[i].type); free(c[i].content); }
    free(c);
    return arr;
}

int db_add_procedure(Database *d, const char *name, const char *steps_json,
                     const char *source_task)
{
    sqlite3_stmt *s = NULL;
    const char *sql = "INSERT INTO procedures(name,steps,source_task,created_at) VALUES(?,?,?,?)"
                      " ON CONFLICT(name) DO UPDATE SET steps=excluded.steps;";
    if (sqlite3_prepare_v2(d->db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    bind_text(s, 1, name); bind_text(s, 2, steps_json); bind_text(s, 3, source_task);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)time(NULL));
    int rc = sqlite3_step(s); sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

cJSON *db_get_procedure(Database *d, const char *name)
{
    sqlite3_stmt *s = NULL;
    const char *sql = "SELECT steps FROM procedures WHERE name=?;";
    if (sqlite3_prepare_v2(d->db, sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    bind_text(s, 1, name);
    cJSON *out = NULL;
    if (sqlite3_step(s) == SQLITE_ROW)
        out = cJSON_CreateString((const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    return out;
}

cJSON *db_get_trace(Database *d, const char *task_id)
{
    sqlite3_stmt *s = NULL;
    const char *sql = "SELECT step_index,kind,detail,created_at FROM trace WHERE task_id=? ORDER BY id;";
    cJSON *arr = cJSON_CreateArray();
    if (sqlite3_prepare_v2(d->db, sql, -1, &s, NULL) != SQLITE_OK) return arr;
    bind_text(s, 1, task_id);
    while (sqlite3_step(s) == SQLITE_ROW) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "step", sqlite3_column_int(s, 0));
        cJSON_AddStringToObject(o, "kind", (const char *)sqlite3_column_text(s, 1));
        const char *det = (const char *)sqlite3_column_text(s, 2);
        cJSON_AddStringToObject(o, "detail", det ? det : "");
        cJSON_AddNumberToObject(o, "ts", (double)sqlite3_column_int64(s, 3));
        cJSON_AddItemToArray(arr, o);
    }
    sqlite3_finalize(s);
    return arr;
}
