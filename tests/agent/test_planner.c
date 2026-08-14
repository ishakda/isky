/* SPDX-License-Identifier: Apache-2.0 */
/* Tests for agent/planner/planner.c: robust plan generation via a mock backend. */
#include "test_util.h"
#include "../../agent/planner/planner.h"
#include "../../agent/model/mock_backend.h"

static void test_valid_plan(void)
{
    ModelBackend *backend = mock_backend_create();
    const char *plan_json =
        "{\"goal\":\"build hello world\",\"steps\":["
        "{\"id\":1,\"description\":\"write hello.c\",\"tool\":\"write_file\","
        "\"expected_result\":\"file created\",\"verification\":\"file_exists:hello.c\"},"
        "{\"id\":2,\"description\":\"compile\",\"tool\":\"shell_execute\","
        "\"expected_result\":\"binary built\",\"verification\":\"exit_zero\"},"
        "{\"id\":3,\"description\":\"run\",\"tool\":\"shell_execute\","
        "\"expected_result\":\"prints hello\",\"verification\":\"contains:hello\"}"
        "]}";
    mock_backend_push(backend, plan_json);

    Planner pl;
    memset(&pl, 0, sizeof pl);
    pl.backend = backend;
    pl.max_attempts = 3;
    pl.temperature = 0.0f;
    pl.max_tokens = 512;
    pl.tools_desc = "write_file, shell_execute";

    Plan plan;
    int ok = planner_make_plan(&pl, "build hello world", NULL, &plan);
    CHECK(ok == 1, "planner_make_plan should succeed with a valid plan");
    CHECK(plan.n_steps == 3, "expected 3 steps, got %d", plan.n_steps);
    CHECK(plan.goal && !strcmp(plan.goal, "build hello world"),
          "goal mismatch: %s", plan.goal ? plan.goal : "(null)");
    CHECK(mock_backend_calls(backend) == 1, "expected exactly one model call");

    plan_free(&plan);
    mock_backend_destroy(backend);
}

static void test_fallback_single_step(void)
{
    ModelBackend *backend = mock_backend_create();
    /* garbage for every attempt: no valid JSON plan can be extracted */
    mock_backend_push(backend, "this is not json at all, sorry!");
    mock_backend_push(backend, "still garbage ### not a plan");
    mock_backend_push(backend, "nope, more garbage <<<>>>");

    Planner pl;
    memset(&pl, 0, sizeof pl);
    pl.backend = backend;
    pl.max_attempts = 3;
    pl.temperature = 0.0f;
    pl.max_tokens = 512;
    pl.tools_desc = "write_file, shell_execute";

    Plan plan;
    int ok = planner_make_plan(&pl, "some goal", NULL, &plan);
    CHECK(ok == 1, "planner_make_plan should fall back rather than fail outright");
    CHECK(plan.n_steps == 1, "fallback plan should have exactly 1 step, got %d", plan.n_steps);
    CHECK(mock_backend_calls(backend) == 3, "should have exhausted all 3 attempts");

    plan_free(&plan);
    mock_backend_destroy(backend);
}

int main(void)
{
    test_valid_plan();
    test_fallback_single_step();
    return test_report("test_planner");
}
