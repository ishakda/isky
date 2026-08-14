/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include "memory.h"
#include "../util/buf.h"

#include <stdlib.h>
#include <string.h>

void memory_init(Memory *m, Database *db, const char *task_id, int cap)
{
    memset(m, 0, sizeof *m);
    m->db = db;
    m->task_id = task_id;
    m->st.cap = cap > 0 ? cap : 12;
    m->st.recent = (char **)calloc((size_t)m->st.cap, sizeof(char *));
}

void memory_free(Memory *m)
{
    if (!m) return;
    free(m->st.goal);
    free(m->st.plan);
    for (int i = 0; i < m->st.count; i++) free(m->st.recent[i]);
    free(m->st.recent);
    memset(m, 0, sizeof *m);
}

void memory_set_goal(Memory *m, const char *goal)
{
    free(m->st.goal);
    m->st.goal = goal ? strdup(goal) : NULL;
}

void memory_set_plan(Memory *m, const char *plan)
{
    free(m->st.plan);
    m->st.plan = plan ? strdup(plan) : NULL;
}

void memory_add_observation(Memory *m, const char *obs)
{
    if (!obs) return;
    ShortTerm *s = &m->st;
    if (s->count < s->cap) {
        s->recent[s->count++] = strdup(obs);
    } else {
        free(s->recent[s->head]);
        s->recent[s->head] = strdup(obs);
        s->head = (s->head + 1) % s->cap;
    }
    if (m->db && m->task_id)
        db_log_observation(m->db, m->task_id, -1, obs);
}

void memory_record_episode(Memory *m, const char *task, const char *action,
                           const char *result, const char *solution)
{
    if (!m->db) return;
    cJSON *o = cJSON_CreateObject();
    if (task) cJSON_AddStringToObject(o, "task", task);
    if (action) cJSON_AddStringToObject(o, "action", action);
    if (result) cJSON_AddStringToObject(o, "result", result);
    if (solution) cJSON_AddStringToObject(o, "solution", solution);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    db_add_memory(m->db, "episodic", s ? s : "{}", 0.5, m->task_id);
    free(s);
}

void memory_record_fact(Memory *m, const char *fact, double importance)
{
    if (m->db && fact) db_add_memory(m->db, "semantic", fact, importance, m->task_id);
}

void memory_record_procedure(Memory *m, const char *name, const char *steps_json)
{
    if (m->db && name) db_add_procedure(m->db, name, steps_json ? steps_json : "[]", m->task_id);
}

static void append_items(ABuf *b, cJSON *arr, const char *label)
{
    if (!arr) return;
    int n = cJSON_GetArraySize(arr);
    if (n == 0) return;
    ab_printf(b, "%s:\n", label);
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(it, "content");
        if (cJSON_IsString(c)) ab_printf(b, "  - %s\n", c->valuestring);
    }
}

char *memory_recall(Memory *m, const char *query, int max_items)
{
    if (!m->db) return strdup("");
    if (max_items <= 0) max_items = 6;
    ABuf b; ab_init(&b);
    cJSON *sem = db_search_memories(m->db, "semantic", query, max_items);
    cJSON *epi = db_search_memories(m->db, "episodic", query, max_items);
    cJSON *proc = db_search_memories(m->db, "procedural", query, max_items);
    append_items(&b, sem, "Known facts");
    append_items(&b, epi, "Past experiences");
    append_items(&b, proc, "Procedures");
    cJSON_Delete(sem); cJSON_Delete(epi); cJSON_Delete(proc);
    return ab_take(&b);
}

char *memory_recent_observations(Memory *m, int max_chars)
{
    ShortTerm *s = &m->st;
    ABuf b; ab_init(&b);
    /* iterate oldest -> newest */
    for (int i = 0; i < s->count; i++) {
        int idx = (s->count < s->cap) ? i : (s->head + i) % s->cap;
        if (max_chars > 0 && (int)b.len > max_chars) {
            /* keep only the most recent when over budget: restart from a later point */
        }
        ab_printf(&b, "- %s\n", s->recent[idx]);
    }
    /* trim from the front if over budget (keep most recent) */
    if (max_chars > 0 && (int)b.len > max_chars) {
        size_t drop = b.len - (size_t)max_chars;
        char *nl = memchr(b.data + drop, '\n', b.len - drop);
        size_t start = nl ? (size_t)(nl - b.data) + 1 : drop;
        char *out = strdup(b.data + start);
        ab_free(&b);
        return out;
    }
    return ab_take(&b);
}
