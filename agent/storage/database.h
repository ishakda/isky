/* SPDX-License-Identifier: Apache-2.0 */
/* database.h - SQLite persistence (build prompt §14). Tasks, steps, tool calls,
 * observations, memories, procedures, files, trace. Enables crash-safe resume. */
#ifndef AGENT_DATABASE_H
#define AGENT_DATABASE_H

#include "../core/task.h"
#include "cJSON.h"

typedef struct Database Database;

/* Opens (creating if needed) and applies the schema. ":memory:" is valid. */
Database *db_open(const char *path);
void      db_close(Database *d);

/* Tasks. Upsert persists the full task row; load restores it. */
int  db_save_task(Database *d, const AgentTask *t);
/* Loads into *t (which must be zeroed/fresh). Returns 1 found, 0 not, -1 error. */
int  db_load_task(Database *d, const char *task_id, AgentTask *t);
/* Newest tasks first. Returns a JSON array (caller cJSON_Delete). */
cJSON *db_list_tasks(Database *d, int limit);

/* Append-only logs. */
int  db_log_step(Database *d, const char *task_id, int idx, const char *desc,
                 const char *tool, const char *status);
int  db_log_tool_call(Database *d, const char *task_id, int idx, const char *tool,
                      const char *args_json, int ok, const char *error_class);
int  db_log_observation(Database *d, const char *task_id, int idx, const char *content);
int  db_log_message(Database *d, const char *task_id, const char *role, const char *content);
int  db_log_trace(Database *d, const char *task_id, int idx, const char *kind,
                  const char *detail);
int  db_log_file(Database *d, const char *task_id, const char *path, const char *action);

/* Memories. */
int  db_add_memory(Database *d, const char *type, const char *content,
                   double importance, const char *source_task);
/* Retrieve up to `limit` memories of `type` (NULL = any) most relevant to `query`
 * (keyword overlap * importance * recency). Returns JSON array (caller frees). */
cJSON *db_search_memories(Database *d, const char *type, const char *query, int limit);
int  db_add_procedure(Database *d, const char *name, const char *steps_json,
                      const char *source_task);
cJSON *db_get_procedure(Database *d, const char *name);

/* The execution trace for a task, as a JSON array in order. */
cJSON *db_get_trace(Database *d, const char *task_id);

#endif
