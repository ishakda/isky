/* SPDX-License-Identifier: Apache-2.0 */
/* Tests for sandbox confinement/backup/rollback and the permissions policy. */
#include "test_util.h"
#include "../../agent/security/sandbox.h"
#include "../../agent/security/permissions.h"
#include "../../agent/tools/tool_registry.h"
#include "../../agent/util/platform.h"
#include "../../agent/util/buf.h"

#include <sys/types.h>
#include <unistd.h>

static int always_confirm(const char *tool, SecurityLevel lvl, const char *summary, void *ud)
{
    (void)tool; (void)lvl; (void)summary; (void)ud;
    return 1;
}

static void test_sandbox_resolve(void)
{
    char workspace[256];
    snprintf(workspace, sizeof workspace, "/tmp/k3agent_sec_test_%d", (int)getpid());
    Sandbox *s = sandbox_create(workspace, 1);

    char out[4096];
    CHECK(sandbox_resolve(s, "../etc/passwd", out, sizeof out) == -1,
          "sandbox_resolve should reject traversal via ../etc/passwd");
    CHECK(sandbox_resolve(s, "/etc/passwd", out, sizeof out) == -1,
          "sandbox_resolve should reject absolute path outside root");
    CHECK(sandbox_resolve(s, "notes.txt", out, sizeof out) == 0,
          "sandbox_resolve should accept a normal relative path");

    sandbox_destroy(s);
}

static void test_sandbox_backup_rollback(void)
{
    char workspace[256];
    snprintf(workspace, sizeof workspace, "/tmp/k3agent_sec_test2_%d", (int)getpid());
    Sandbox *s = sandbox_create(workspace, 1);

    char abspath[4096];
    CHECK(sandbox_resolve(s, "data.txt", abspath, sizeof abspath) == 0, "resolve data.txt");

    const char *original = "original content\n";
    CHECK(write_entire_file(abspath, original, strlen(original)) == 0, "write original file");

    CHECK(sandbox_backup(s, abspath) == 0, "backup should succeed");

    const char *modified = "MODIFIED CONTENT - overwritten\n";
    CHECK(write_entire_file(abspath, modified, strlen(modified)) == 0, "write modified content");

    size_t len = 0;
    char *check1 = read_entire_file(abspath, 0, &len, NULL);
    CHECK(check1 && !strcmp(check1, modified), "file should now hold modified content");
    free(check1);

    CHECK(sandbox_rollback(s, abspath) == 0, "rollback should succeed");

    char *check2 = read_entire_file(abspath, 0, &len, NULL);
    CHECK(check2 && !strcmp(check2, original),
          "rollback should restore original content, got: %s", check2 ? check2 : "(null)");
    free(check2);

    sandbox_destroy(s);
}

static void test_permissions_confirmation(void)
{
    Permissions *always = permissions_create(APPROVAL_ALWAYS, 4);
    CHECK(permissions_needs_confirmation(always, "read_file", SEC_SAFE) == 1,
          "APPROVAL_ALWAYS should require confirmation even for SEC_SAFE");
    permissions_destroy(always);

    Permissions *never = permissions_create(APPROVAL_NEVER, 4);
    CHECK(permissions_needs_confirmation(never, "shell_execute", SEC_MEDIUM_RISK) == 0,
          "APPROVAL_NEVER should never require confirmation");
    CHECK(permissions_needs_confirmation(never, "delete_file", SEC_HIGH_RISK) == 0,
          "APPROVAL_NEVER should never require confirmation, even for HIGH_RISK");
    permissions_destroy(never);
}

static void test_permissions_autonomy(void)
{
    Permissions *p = permissions_create(APPROVAL_RISKY, 2);
    CHECK(permissions_autonomy_allows(p, SEC_SAFE) == 1,
          "autonomy level 2 should allow SEC_SAFE");
    CHECK(permissions_autonomy_allows(p, SEC_MEDIUM_RISK) == 0,
          "autonomy level 2 should NOT allow SEC_MEDIUM_RISK");
    permissions_destroy(p);
}

static void test_permissions_disabled_tool(void)
{
    Permissions *p = permissions_create(APPROVAL_RISKY, 3);
    CHECK(permissions_is_enabled(p, "shell_execute") == 1,
          "tool should default to enabled");
    permissions_set(p, "shell_execute", 0, 0);
    CHECK(permissions_is_enabled(p, "shell_execute") == 0,
          "permissions_set with enabled=0 should disable the tool");
    permissions_destroy(p);
}

static void test_shell_denylist(void)
{
    char workspace[256];
    snprintf(workspace, sizeof workspace, "/tmp/k3agent_sec_test3_%d", (int)getpid());
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
    ctx.task_id = "t1";

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "command", "rm -rf /");
    ToolResult res; tool_result_init(&res);
    tool_registry_execute(reg, "shell_execute", args, &ctx, &res);
    CHECK(res.ok == 0, "shell_execute should refuse 'rm -rf /'");
    cJSON_Delete(args);
    tool_result_free(&res);

    permissions_destroy(perm);
    sandbox_destroy(sandbox);
    tool_registry_destroy(reg);
}

int main(void)
{
    test_sandbox_resolve();
    test_sandbox_backup_rollback();
    test_permissions_confirmation();
    test_permissions_autonomy();
    test_permissions_disabled_tool();
    test_shell_denylist();
    return test_report("test_security");
}
