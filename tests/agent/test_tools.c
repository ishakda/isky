/* SPDX-License-Identifier: Apache-2.0 */
/* Integration tests for the tool registry + built-in filesystem tools. */
#include "test_util.h"
#include "../../agent/tools/tool_registry.h"
#include "../../agent/security/sandbox.h"
#include "../../agent/security/permissions.h"
#include "../../agent/reasoning/verification.h"

#include <sys/types.h>
#include <unistd.h>

static int always_confirm(const char *tool, SecurityLevel lvl, const char *summary, void *ud)
{
    (void)tool; (void)lvl; (void)summary; (void)ud;
    return 1;
}

static void make_workspace(char *out, size_t outsz)
{
    snprintf(out, outsz, "/tmp/k3agent_test_%d", (int)getpid());
}

int main(void)
{
    char workspace[256];
    make_workspace(workspace, sizeof workspace);

    ToolRegistry *reg = tool_registry_create();
    tool_registry_register_builtins(reg);

    Sandbox *sandbox = sandbox_create(workspace, 1);
    Permissions *perm = permissions_create(APPROVAL_NEVER, 4);

    ToolContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.workspace = sandbox_root(sandbox);
    ctx.sandbox_enabled = 1;
    ctx.sandbox = sandbox;
    ctx.permissions = perm;
    ctx.confirm = always_confirm;
    ctx.confirm_ud = NULL;
    ctx.task_id = "t1";

    /* write_file then read_file round-trip */
    cJSON *wargs = cJSON_CreateObject();
    cJSON_AddStringToObject(wargs, "path", "hello.txt");
    cJSON_AddStringToObject(wargs, "content", "hello world\n");
    ToolResult wres; tool_result_init(&wres);
    int rc = tool_registry_execute(reg, "write_file", wargs, &ctx, &wres);
    CHECK(rc == 0, "write_file dispatch failed");
    CHECK(wres.ok == 1, "write_file should succeed: %s", wres.error ? wres.error : "?");
    cJSON_Delete(wargs);
    tool_result_free(&wres);

    cJSON *rargs = cJSON_CreateObject();
    cJSON_AddStringToObject(rargs, "path", "hello.txt");
    ToolResult rres; tool_result_init(&rres);
    rc = tool_registry_execute(reg, "read_file", rargs, &ctx, &rres);
    CHECK(rc == 0, "read_file dispatch failed");
    CHECK(rres.ok == 1, "read_file should succeed: %s", rres.error ? rres.error : "?");
    CHECK(rres.output && strstr(rres.output, "hello world") != NULL,
          "read_file output should contain written content, got: %s",
          rres.output ? rres.output : "(null)");
    cJSON_Delete(rargs);
    tool_result_free(&rres);

    /* edit_file replacing a unique block */
    cJSON *eargs = cJSON_CreateObject();
    cJSON_AddStringToObject(eargs, "path", "hello.txt");
    cJSON_AddStringToObject(eargs, "old", "hello world");
    cJSON_AddStringToObject(eargs, "new", "goodbye world");
    ToolResult eres; tool_result_init(&eres);
    rc = tool_registry_execute(reg, "edit_file", eargs, &ctx, &eres);
    CHECK(rc == 0, "edit_file dispatch failed");
    CHECK(eres.ok == 1, "edit_file should succeed: %s", eres.error ? eres.error : "?");
    cJSON_Delete(eargs);
    tool_result_free(&eres);

    /* verify the edit actually happened */
    cJSON *rargs2 = cJSON_CreateObject();
    cJSON_AddStringToObject(rargs2, "path", "hello.txt");
    ToolResult rres2; tool_result_init(&rres2);
    tool_registry_execute(reg, "read_file", rargs2, &ctx, &rres2);
    CHECK(rres2.output && strstr(rres2.output, "goodbye world") != NULL,
          "edit_file did not update content: %s", rres2.output ? rres2.output : "(null)");
    CHECK(rres2.output && strstr(rres2.output, "hello world") == NULL,
          "old content should be gone after edit");
    cJSON_Delete(rargs2);
    tool_result_free(&rres2);

    /* edit_file failing when the old block is absent */
    cJSON *eargs2 = cJSON_CreateObject();
    cJSON_AddStringToObject(eargs2, "path", "hello.txt");
    cJSON_AddStringToObject(eargs2, "old", "this text is not in the file");
    cJSON_AddStringToObject(eargs2, "new", "replacement");
    ToolResult eres2; tool_result_init(&eres2);
    tool_registry_execute(reg, "edit_file", eargs2, &ctx, &eres2);
    CHECK(eres2.ok == 0, "edit_file should fail when 'old' block is absent");
    CHECK(eres2.error_class == REC_INVALID_ARGUMENT,
          "expected REC_INVALID_ARGUMENT, got %d", eres2.error_class);
    cJSON_Delete(eargs2);
    tool_result_free(&eres2);

    /* list_directory lists the created file */
    cJSON *largs = cJSON_CreateObject();
    ToolResult lres; tool_result_init(&lres);
    rc = tool_registry_execute(reg, "list_directory", largs, &ctx, &lres);
    CHECK(rc == 0, "list_directory dispatch failed");
    CHECK(lres.ok == 1, "list_directory should succeed");
    CHECK(lres.output && strstr(lres.output, "hello.txt") != NULL,
          "list_directory should show hello.txt: %s", lres.output ? lres.output : "(null)");
    cJSON_Delete(largs);
    tool_result_free(&lres);

    /* read_file on a missing file returns ok=0, error_class REC_FILE_NOT_FOUND */
    cJSON *margs = cJSON_CreateObject();
    cJSON_AddStringToObject(margs, "path", "does_not_exist.txt");
    ToolResult mres; tool_result_init(&mres);
    rc = tool_registry_execute(reg, "read_file", margs, &ctx, &mres);
    CHECK(rc == 0, "read_file dispatch (missing file) should still return 0");
    CHECK(mres.ok == 0, "read_file on missing file should fail");
    CHECK(mres.error_class == REC_FILE_NOT_FOUND,
          "expected REC_FILE_NOT_FOUND, got %d", mres.error_class);
    cJSON_Delete(margs);
    tool_result_free(&mres);

    permissions_destroy(perm);
    sandbox_destroy(sandbox);
    tool_registry_destroy(reg);
    return test_report("test_tools");
}
