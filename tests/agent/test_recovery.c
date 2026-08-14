/* SPDX-License-Identifier: Apache-2.0 */
/* Tests for agent/reasoning/verification.c (recovery strategy + verify_check),
 * plus an integration test that the agent loop survives malformed model output. */
#include "test_util.h"
#include "../../agent/reasoning/verification.h"
#include "../../agent/core/agent.h"
#include "../../agent/core/agent_loop.h"
#include "../../agent/model/mock_backend.h"
#include "../../agent/util/platform.h"
#include "../../agent/util/buf.h"

#include <sys/types.h>
#include <unistd.h>

static int always_confirm(const char *tool, SecurityLevel lvl, const char *summary, void *ud)
{
    (void)tool; (void)lvl; (void)summary; (void)ud;
    return 1;
}

static void test_recovery_strategy(void)
{
    /* transient timeout: retry while under max_retries, then replan */
    CHECK(recovery_strategy_for(REC_TIMEOUT, 0, 3) == STRAT_RETRY,
          "REC_TIMEOUT with attempts<max should retry");
    CHECK(recovery_strategy_for(REC_TIMEOUT, 2, 3) == STRAT_RETRY,
          "REC_TIMEOUT at attempts=2<3 should still retry");
    CHECK(recovery_strategy_for(REC_TIMEOUT, 3, 3) == STRAT_REPLAN,
          "REC_TIMEOUT past max_retries should replan");

    CHECK(recovery_strategy_for(REC_PERMISSION_ERROR, 0, 3) == STRAT_ASK_USER,
          "REC_PERMISSION_ERROR should always ask the user");

    CHECK(recovery_strategy_for(REC_COMPILATION_ERROR, 0, 3) == STRAT_REPLAN,
          "REC_COMPILATION_ERROR should replan");
}

static void test_verify_check_file_exists(void)
{
    char dir[256];
    snprintf(dir, sizeof dir, "/tmp/k3agent_recov_test_%d", (int)getpid());
    fs_mkdir_p(dir);
    char filepath[512];
    snprintf(filepath, sizeof filepath, "%s/exists.txt", dir);
    write_entire_file(filepath, "content", 7);

    VerifyCheck vc_exists = { "file_exists", "exists.txt" };
    char *reason = NULL;
    int ok = verify_check(&vc_exists, dir, NULL, 0, &reason);
    CHECK(ok == 1, "file_exists should pass for a file that exists");
    CHECK(reason == NULL, "reason should be NULL on success");

    VerifyCheck vc_missing = { "file_exists", "missing.txt" };
    reason = NULL;
    ok = verify_check(&vc_missing, dir, NULL, 0, &reason);
    CHECK(ok == 0, "file_exists should fail for a missing file");
    CHECK(reason != NULL, "reason should be set on failure");
    free(reason);
}

static void test_verify_check_exit_zero(void)
{
    VerifyCheck vc = { "exit_zero", NULL };
    char *reason = NULL;
    CHECK(verify_check(&vc, ".", NULL, 0, &reason) == 1, "exit_zero with code 0 should pass");
    free(reason); reason = NULL;
    CHECK(verify_check(&vc, ".", NULL, 1, &reason) == 0, "exit_zero with nonzero code should fail");
    CHECK(reason != NULL, "reason should be set for nonzero exit");
    free(reason);
}

static void test_verify_check_contains(void)
{
    VerifyCheck vc = { "contains", "HELLO" };
    char *reason = NULL;
    CHECK(verify_check(&vc, ".", "the program printed hello world", 0, &reason) == 1,
          "contains should match case-insensitively");
    free(reason); reason = NULL;
    CHECK(verify_check(&vc, ".", "the program printed goodbye", 0, &reason) == 0,
          "contains should fail when substring absent");
    free(reason);
}

static void test_loop_survives_garbage(void)
{
    char workspace[256];
    snprintf(workspace, sizeof workspace, "/tmp/k3agent_recov_loop_%d", (int)getpid());

    AgentConfig cfg;
    agent_config_defaults(&cfg);
    snprintf(cfg.backend, sizeof cfg.backend, "mock");
    snprintf(cfg.workspace, sizeof cfg.workspace, "%s", workspace);
    cfg.memory_enabled = 1;
    snprintf(cfg.database, sizeof cfg.database, ":memory:");
    cfg.approval = APPROVAL_NEVER;
    cfg.autonomy_level = 4;
    cfg.max_iterations = 15;
    cfg.max_retries = 3;

    ModelBackend *backend = mock_backend_create();
    /* planner call: give a valid one-step plan so we get straight to the loop */
    mock_backend_push(backend,
        "{\"goal\":\"say hi\",\"steps\":["
        "{\"id\":1,\"description\":\"answer\",\"tool\":\"\"}]}");
    /* a couple of garbage / invalid actions the loop must recover from */
    mock_backend_push(backend, "no valid json here at all");
    mock_backend_push(backend, "{{{ not json either ]]]");
    /* finally a valid final action */
    mock_backend_push(backend, "{\"action\":\"final\",\"answer\":\"hi there\"}");

    Agent *a = agent_create(&cfg, backend);
    CHECK(a != NULL, "agent_create failed");
    if (!a) { mock_backend_destroy(backend); return; }
    a->confirm = always_confirm;

    AgentTask *task = task_create("say hi", cfg.max_iterations);
    RunOptions opt; memset(&opt, 0, sizeof opt);
    RunOutcome outcome = agent_loop_run(a, task, &opt);

    CHECK(outcome == RUN_COMPLETED,
          "loop should recover from garbage actions and complete, got outcome %d", outcome);
    CHECK(task->final_answer && !strcmp(task->final_answer, "hi there"),
          "final answer mismatch: %s", task->final_answer ? task->final_answer : "(null)");

    task_free(task);
    agent_destroy(a);
    mock_backend_destroy(backend);
}

int main(void)
{
    test_recovery_strategy();
    test_verify_check_file_exists();
    test_verify_check_exit_zero();
    test_verify_check_contains();
    test_loop_survives_garbage();
    return test_report("test_recovery");
}
