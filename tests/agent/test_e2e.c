/* SPDX-License-Identifier: Apache-2.0 */
/* End-to-end test: a scripted mock backend drives the full agent loop through
 * plan -> write hello.c -> compile -> run -> final, using REAL tools (no model
 * inference). Verifies the plumbing end to end without needing the k3 backend. */
#include "test_util.h"
#include "../../agent/core/agent.h"
#include "../../agent/core/agent_loop.h"
#include "../../agent/model/mock_backend.h"
#include "../../agent/util/platform.h"

#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>

static int always_confirm(const char *tool, SecurityLevel lvl, const char *summary, void *ud)
{
    (void)tool; (void)lvl; (void)summary; (void)ud;
    return 1;
}

static int compiler_available(const char **out_cc)
{
    const char *candidates[] = { "cc", "gcc", "clang", NULL };
    for (int i = 0; candidates[i]; i++) {
        char cmd[128];
        snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", candidates[i]);
        if (system(cmd) == 0) { *out_cc = candidates[i]; return 1; }
    }
    return 0;
}

int main(void)
{
    char workspace[256];
    snprintf(workspace, sizeof workspace, "/tmp/k3agent_e2e_test_%d", (int)getpid());

    const char *cc = "cc";
    int have_cc = compiler_available(&cc);

    AgentConfig cfg;
    agent_config_defaults(&cfg);
    snprintf(cfg.backend, sizeof cfg.backend, "mock");
    snprintf(cfg.workspace, sizeof cfg.workspace, "%s", workspace);
    cfg.memory_enabled = 1;
    snprintf(cfg.database, sizeof cfg.database, ":memory:");
    cfg.approval = APPROVAL_NEVER;
    cfg.autonomy_level = 4;
    cfg.max_iterations = 20;
    cfg.max_retries = 3;

    ModelBackend *backend = mock_backend_create();

    /* 1) planner: three-step plan */
    mock_backend_push(backend,
        "{\"goal\":\"compile and run hello world\",\"steps\":["
        "{\"id\":1,\"description\":\"write hello.c\",\"tool\":\"write_file\","
        "\"verification\":\"file_exists:hello.c\"},"
        "{\"id\":2,\"description\":\"compile\",\"tool\":\"shell_execute\","
        "\"verification\":\"exit_zero\"},"
        "{\"id\":3,\"description\":\"run\",\"tool\":\"shell_execute\","
        "\"verification\":\"contains:hello\"}"
        "]}");
    /* 2) write hello.c */
    mock_backend_push(backend,
        "{\"action\":\"tool\",\"tool\":\"write_file\",\"arguments\":{"
        "\"path\":\"hello.c\","
        "\"content\":\"#include <stdio.h>\\nint main(void){printf(\\\"hello\\\\n\\\");return 0;}\\n\""
        "}}");
    /* 3) compile */
    char compile_action[256];
    snprintf(compile_action, sizeof compile_action,
        "{\"action\":\"tool\",\"tool\":\"shell_execute\",\"arguments\":{"
        "\"command\":\"%s hello.c -o hello\"}}", cc);
    mock_backend_push(backend, compile_action);
    /* 4) run */
    mock_backend_push(backend,
        "{\"action\":\"tool\",\"tool\":\"shell_execute\",\"arguments\":{"
        "\"command\":\"./hello\"}}");
    /* 5) final */
    mock_backend_push(backend,
        "{\"action\":\"final\",\"answer\":\"Compiled and ran hello.c; it printed hello.\"}");

    Agent *a = agent_create(&cfg, backend);
    CHECK(a != NULL, "agent_create failed");
    if (!a) { mock_backend_destroy(backend); return test_report("test_e2e"); }
    a->confirm = always_confirm;

    AgentTask *task = task_create("compile and run hello world", cfg.max_iterations);
    RunOptions opt; memset(&opt, 0, sizeof opt);
    RunOutcome outcome = agent_loop_run(a, task, &opt);

    CHECK(outcome == RUN_COMPLETED, "expected RUN_COMPLETED, got %d", outcome);

    char c_path[512];
    snprintf(c_path, sizeof c_path, "%s/hello.c", workspace);
    CHECK(fs_exists(c_path), "hello.c should have been created at %s", c_path);

    if (have_cc) {
        char bin_path[512];
        snprintf(bin_path, sizeof bin_path, "%s/hello", workspace);
        CHECK(fs_exists(bin_path),
              "hello binary should exist at %s when a C compiler (%s) is available",
              bin_path, cc);
    } else {
        fprintf(stderr, "  NOTE: no C compiler found on PATH; skipping binary-existence check\n");
    }

    task_free(task);
    agent_destroy(a);
    mock_backend_destroy(backend);
    return test_report("test_e2e");
}
