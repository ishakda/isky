/* SPDX-License-Identifier: Apache-2.0 */
/* memory.h - the four memory types (build prompt §13). Short-term is the in-RAM
 * working set for the current task; episodic/semantic/procedural persist in SQLite.
 * Retrieval is by relevance, and NOT every message is stored (§14). */
#ifndef AGENT_MEMORY_H
#define AGENT_MEMORY_H

#include "../storage/database.h"
#include "cJSON.h"

/* Short-term memory: a bounded ring of recent observations for the current task,
 * plus the goal and current plan. Lives in RAM only. */
typedef struct {
    char  *goal;
    char  *plan;
    char **recent;       /* ring of recent observation strings */
    int    cap, head, count;
} ShortTerm;

typedef struct {
    Database  *db;       /* NULL disables persistence (memory still works in RAM) */
    ShortTerm  st;
    const char *task_id;
} Memory;

void memory_init(Memory *m, Database *db, const char *task_id, int short_term_cap);
void memory_free(Memory *m);

void memory_set_goal(Memory *m, const char *goal);
void memory_set_plan(Memory *m, const char *plan_json);
void memory_add_observation(Memory *m, const char *obs);   /* short-term ring */

/* Persistent memory (episodic: what happened; semantic: facts; procedural: how-to). */
void memory_record_episode(Memory *m, const char *task, const char *action,
                           const char *result, const char *solution);
void memory_record_fact(Memory *m, const char *fact, double importance);
void memory_record_procedure(Memory *m, const char *name, const char *steps_json);

/* Build a compact "RELEVANT MEMORY" block for the given query. Caller frees.
 * Pulls the most relevant semantic + episodic + procedural items under a budget. */
char *memory_recall(Memory *m, const char *query, int max_items);

/* Short-term recent observations joined newest-last. Caller frees. */
char *memory_recent_observations(Memory *m, int max_chars);

#endif
