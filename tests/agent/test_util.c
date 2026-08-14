/* SPDX-License-Identifier: Apache-2.0 */
/* Unit tests for util (buf, jsonx, log redaction) and config. */
#include "test_util.h"
#include "../../agent/util/buf.h"
#include "../../agent/util/jsonx.h"
#include "../../agent/util/log.h"
#include "../../agent/core/config.h"
#include "../../agent/core/state.h"

static void test_buf(void)
{
    ABuf b; ab_init(&b);
    ab_puts(&b, "hello");
    ab_putc(&b, ' ');
    ab_printf(&b, "%s %d", "world", 42);
    CHECK_STR_EQ(b.data, "hello world 42");
    CHECK(b.len == 14, "len %zu", b.len);
    ab_free(&b);
}

static void test_jsonx_extract(void)
{
    const char *msg = "Sure! Here is the action:\n```json\n"
                      "{\"action\":\"tool\",\"tool\":\"read_file\",\"arguments\":{\"path\":\"a.c\"}}\n"
                      "```\nHope that helps.";
    cJSON *o = jx_extract_object(msg, NULL, NULL);
    CHECK(o != NULL, "extract failed");
    if (o) {
        CHECK_STR_EQ(jx_str(o, "action", "?"), "tool");
        CHECK_STR_EQ(jx_str(o, "tool", "?"), "read_file");
        cJSON *args = jx_obj(o, "arguments");
        CHECK(args != NULL, "args missing");
        if (args) CHECK_STR_EQ(jx_str(args, "path", "?"), "a.c");
        cJSON_Delete(o);
    }
}

static void test_jsonx_repair(void)
{
    /* trailing comma, single quotes, Python literals, unquoted key */
    const char *bad = "{action: 'final', answer: 'done', ok: True,}";
    cJSON *o = jx_repair_object(bad);
    CHECK(o != NULL, "repair failed");
    if (o) {
        CHECK_STR_EQ(jx_str(o, "action", "?"), "final");
        CHECK_STR_EQ(jx_str(o, "answer", "?"), "done");
        CHECK(jx_bool(o, "ok", 0) == 1, "bool repair");
        cJSON_Delete(o);
    }
}

static void test_redact(void)
{
    /* The fake secret is assembled at runtime from harmless pieces so this source
     * file contains no literal that resembles a real credential (which would trip
     * repository secret-scanning push protection). The redactor still sees the full
     * "sk-..."-style value at run time. */
    char in[128];
    snprintf(in, sizeof in, "api_key=%s%s and normal text", "sk", "-abcdef123456");
    char *r = log_redact(in);
    CHECK(strstr(r, "abcdef") == NULL, "secret leaked: %s", r);
    CHECK(strstr(r, "normal text") != NULL, "lost normal text: %s", r);
    free(r);
    char *r2 = log_redact("Authorization: Bearer topsecrettoken");
    CHECK(strstr(r2, "topsecrettoken") == NULL, "bearer leaked: %s", r2);
    free(r2);
}

static void test_config(void)
{
    AgentConfig c;
    agent_config_defaults(&c);
    CHECK(c.max_iterations == 30, "default iters");
    CHECK(c.approval == APPROVAL_RISKY, "default approval");
    CHECK(c.autonomy_level == 3, "default autonomy");
    CHECK(agent_config_parse_approval("never") == APPROVAL_NEVER, "parse never");
    CHECK_STR_EQ(agent_config_approval_name(APPROVAL_ALWAYS), "always");
}

static void test_state_machine(void)
{
    AgentState s = AGENT_IDLE;
    CHECK(agent_state_transition(&s, AGENT_ANALYZING) == 1, "idle->analyzing");
    CHECK(agent_state_transition(&s, AGENT_PLANNING) == 1, "analyzing->planning");
    CHECK(agent_state_transition(&s, AGENT_EXECUTING) == 1, "planning->executing");
    /* illegal jump */
    AgentState c = AGENT_COMPLETED;
    CHECK(agent_state_transition(&c, AGENT_EXECUTING) == 0, "completed is terminal");
    CHECK(agent_state_is_terminal(AGENT_FAILED), "failed terminal");
}

int main(void)
{
    test_buf();
    test_jsonx_extract();
    test_jsonx_repair();
    test_redact();
    test_config();
    test_state_machine();
    return test_report("test_util");
}
