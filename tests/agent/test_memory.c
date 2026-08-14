/* SPDX-License-Identifier: Apache-2.0 */
/* Tests for agent/memory/memory.c and its interaction with database.c. */
#include "test_util.h"
#include "../../agent/memory/memory.h"
#include "../../agent/storage/database.h"

static void test_memory_recall_and_observations(void)
{
    Database *db = db_open(":memory:");
    CHECK(db != NULL, "db_open(:memory:) failed");

    Memory mem;
    memory_init(&mem, db, "task-1", 8);
    memory_set_goal(&mem, "build a rocket");

    memory_record_fact(&mem, "the fuel tank capacity is 500 liters", 0.9);
    memory_record_fact(&mem, "the launchpad is at site 7", 0.6);
    memory_record_fact(&mem, "unrelated fact about bananas", 0.2);

    memory_add_observation(&mem, "checked fuel tank: OK");
    memory_add_observation(&mem, "checked launchpad: clear");
    memory_add_observation(&mem, "ignition sequence started");

    char *recall = memory_recall(&mem, "fuel tank capacity", 6);
    CHECK(recall != NULL, "memory_recall returned NULL");
    CHECK(recall && strstr(recall, "fuel tank") != NULL,
          "memory_recall should surface the fuel tank fact, got: %s",
          recall ? recall : "(null)");
    free(recall);

    char *obs = memory_recent_observations(&mem, 4000);
    CHECK(obs != NULL, "memory_recent_observations returned NULL");
    CHECK(obs && strstr(obs, "ignition sequence started") != NULL,
          "recent observations should include the last one, got: %s",
          obs ? obs : "(null)");
    CHECK(obs && strstr(obs, "checked fuel tank") != NULL,
          "recent observations should include earlier ones too, got: %s",
          obs ? obs : "(null)");
    free(obs);

    memory_free(&mem);
    db_close(db);
}

static void test_db_memory_directly(void)
{
    Database *db = db_open(":memory:");
    CHECK(db != NULL, "db_open(:memory:) failed");

    CHECK(db_add_memory(db, "semantic", "fact one about widgets", 0.5, "t1") == 0,
          "db_add_memory failed (1)");
    CHECK(db_add_memory(db, "semantic", "fact two about gadgets", 0.5, "t1") == 0,
          "db_add_memory failed (2)");
    CHECK(db_add_memory(db, "semantic", "fact three about widgets and gears", 0.5, "t1") == 0,
          "db_add_memory failed (3)");

    cJSON *results = db_search_memories(db, "semantic", "widgets", 2);
    CHECK(results != NULL, "db_search_memories returned NULL");
    CHECK(cJSON_IsArray(results), "db_search_memories should return a JSON array");
    CHECK(cJSON_GetArraySize(results) == 2,
          "expected 2 results (limit), got %d", cJSON_GetArraySize(results));
    cJSON_Delete(results);

    cJSON *all = db_search_memories(db, "semantic", "widgets", 10);
    CHECK(cJSON_GetArraySize(all) == 3,
          "expected all 3 semantic memories when limit exceeds count, got %d",
          cJSON_GetArraySize(all));
    cJSON_Delete(all);

    db_close(db);
}

int main(void)
{
    test_memory_recall_and_observations();
    test_db_memory_directly();
    return test_report("test_memory");
}
