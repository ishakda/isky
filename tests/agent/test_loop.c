/* SPDX-License-Identifier: Apache-2.0 */
/* Integration test for agent/core/agent_loop.c: a scripted mock backend drives a
 * plan -> write_file -> final sequence through the real agent + real tools. */
#include "test_util.h"
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

int main(void)
{
    char workspace[256];
    snprintf(workspace, sizeof workspace, "/tmp/k3agent_loop_test_%d", (int)getpid());

    AgentConfig cfg;
    agent_config_defaults(&cfg);
    snprintf(cfg.backend, sizeof cfg.backend, "mock");
    snprintf(cfg.workspace, sizeof cfg.workspace, "%s", workspace);
    cfg.memory_enabled = 1;
    snprintf(cfg.database, sizeof cfg.database, ":memory:");
    cfg.approval = APPROVAL_NEVER;
    cfg.autonomy_level = 4;
    cfg.max_iterations = 10;
    cfg.max_retries = 3;

    ModelBackend *backend = mock_backend_create();

    /* 1) planner call: a one-step plan to write a file */
    mock_backend_push(backend,
        "{\"goal\":\"write greeting file\",\"steps\":["
        "{\"id\":1,\"description\":\"write greeting.txt\",\"tool\":\"write_file\","
        "\"expected_result\":\"file created\",\"verification\":\"file_exists:greeting.txt\"}"
        "]}");
    /* 2) loop step 1: the tool action */
    mock_backend_push(backend,
        "{\"action\":\"tool\",\"tool\":\"write_file\","
        "\"arguments\":{\"path\":\"greeting.txt\",\"content\":\"hello from the agent\\n\"}}");
    /* 3) loop step 2: final answer */
    mock_backend_push(backend,
        "{\"action\":\"final\",\"answer\":\"Wrote greeting.txt successfully.\"}");

    Agent *a = agent_create(&cfg, backend);
    CHECK(a != NULL, "agent_create failed");
    if (!a) { mock_backend_destroy(backend); return test_report("test_loop"); }

    a->confirm = always_confirm;
    a->confirm_ud = NULL;

    AgentTask *task = task_create("write greeting file", cfg.max_iterations);
    RunOptions opt; memset(&opt, 0, sizeof opt);

    RunOutcome outcome = agent_loop_run(a, task, &opt);
    CHECK(outcome == RUN_COMPLETED, "expected RUN_COMPLETED, got %d", outcome);
    CHECK(task->final_answer != NULL, "task->final_answer should be set");
    CHECK(task->final_answer && strstr(task->final_answer, "greeting.txt") != NULL,
          "final_answer should mention the file, got: %s",
          task->final_answer ? task->final_answer : "(null)");

    char path[512];
    snprintf(path, sizeof path, "%s/greeting.txt", workspace);
    CHECK(fs_exists(path), "greeting.txt should actually have been created at %s", path);
    if (fs_exists(path)) {
        size_t len = 0;
        char *content = read_entire_file(path, 0, &len, NULL);
        CHECK(content && strstr(content, "hello from the agent") != NULL,
              "file content mismatch: %s", content ? content : "(null)");
        free(content);
    }

    task_free(task);
    agent_destroy(a);
    mock_backend_destroy(backend);
    return test_report("test_loop");
}
