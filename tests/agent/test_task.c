/* SPDX-License-Identifier: Apache-2.0 */
/* Tests for agent/core/task.c and its persistence round-trip via database.c. */
#include "test_util.h"
#include "../../agent/core/task.h"
#include "../../agent/storage/database.h"

static void test_task_create_defaults(void)
{
    AgentTask *t = task_create("write a poem", 10);
    CHECK(t != NULL, "task_create failed");
    CHECK(t->id[0] != 0, "task id should be nonempty");
    CHECK(t->goal && !strcmp(t->goal, "write a poem"), "goal mismatch");
    CHECK(t->state == AGENT_IDLE, "new task should start AGENT_IDLE");
    CHECK(t->max_iterations == 10, "max_iterations mismatch");
    task_free(t);
}

static void test_task_set_plan(void)
{
    AgentTask *t = task_create("do things", 5);
    task_set_plan(t, "{\"goal\":\"do things\",\"steps\":[]}", 3);
    CHECK(t->plan && !strcmp(t->plan, "{\"goal\":\"do things\",\"steps\":[]}"),
          "plan not set correctly");
    CHECK(t->total_steps == 3, "total_steps mismatch, got %d", t->total_steps);
    CHECK(t->current_step == 0, "current_step should reset to 0 on set_plan");
    task_free(t);
}

static void test_db_roundtrip(void)
{
    Database *db = db_open(":memory:");
    CHECK(db != NULL, "db_open(:memory:) failed");

    AgentTask *t = task_create("roundtrip goal", 20);
    task_set_plan(t, "{\"goal\":\"roundtrip goal\",\"steps\":[]}", 2);
    t->state = AGENT_EXECUTING;
    t->current_step = 1;
    t->iteration = 3;

    int rc = db_save_task(db, t);
    CHECK(rc == 0, "db_save_task failed");

    AgentTask fresh; memset(&fresh, 0, sizeof fresh);
    int found = db_load_task(db, t->id, &fresh);
    CHECK(found == 1, "db_load_task should find the saved task");
    CHECK(fresh.goal && !strcmp(fresh.goal, "roundtrip goal"),
          "loaded goal mismatch: %s", fresh.goal ? fresh.goal : "(null)");
    CHECK(fresh.state == AGENT_EXECUTING, "loaded state mismatch, got %d", fresh.state);
    CHECK(fresh.plan && strstr(fresh.plan, "roundtrip goal") != NULL,
          "loaded plan mismatch: %s", fresh.plan ? fresh.plan : "(null)");
    CHECK(fresh.total_steps == 2, "loaded total_steps mismatch, got %d", fresh.total_steps);
    CHECK(fresh.current_step == 1, "loaded current_step mismatch, got %d", fresh.current_step);

    cJSON *list = db_list_tasks(db, 10);
    CHECK(list != NULL, "db_list_tasks returned NULL");
    CHECK(cJSON_IsArray(list) && cJSON_GetArraySize(list) >= 1,
          "db_list_tasks should return at least 1 entry");
    cJSON_Delete(list);

    free(fresh.goal); free(fresh.plan); free(fresh.user_question); free(fresh.final_answer);
    task_free(t);
    db_close(db);
}

int main(void)
{
    test_task_create_defaults();
    test_task_set_plan();
    test_db_roundtrip();
    return test_report("test_task");
}
