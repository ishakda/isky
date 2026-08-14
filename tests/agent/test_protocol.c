/* SPDX-License-Identifier: Apache-2.0 */
/* Tests for agent/planner/protocol.c: parsing and validating model actions. */
#include "test_util.h"
#include "../../agent/planner/protocol.h"

static const char *TOOLS[] = { "read_file", NULL };

static void test_parse_tool_clean(void)
{
    AgentAction a;
    const char *txt = "{\"action\":\"tool\",\"tool\":\"read_file\","
                      "\"arguments\":{\"path\":\"a.c\"}}";
    ActionType t = agent_action_parse(txt, &a);
    CHECK(t == ACT_TOOL, "expected ACT_TOOL, got %d", t);
    CHECK(a.tool && !strcmp(a.tool, "read_file"), "tool name mismatch");
    CHECK(a.arguments != NULL, "arguments missing");
    CHECK(agent_action_validate(&a, TOOLS) == 1, "validate failed: %s", a.error);
    agent_action_free(&a);
}

static void test_parse_tool_in_prose(void)
{
    AgentAction a;
    const char *txt =
        "Sure! Here's the action:\n```json\n"
        "{\"action\":\"tool\",\"tool\":\"read_file\",\"arguments\":{\"path\":\"b.c\"}}\n"
        "```\nHope that helps.";
    ActionType t = agent_action_parse(txt, &a);
    CHECK(t == ACT_TOOL, "expected ACT_TOOL from fenced JSON, got %d", t);
    CHECK(a.tool && !strcmp(a.tool, "read_file"), "tool name mismatch (prose)");
    agent_action_free(&a);
}

static void test_parse_final(void)
{
    AgentAction a;
    const char *txt = "{\"action\":\"final\",\"answer\":\"all done\"}";
    ActionType t = agent_action_parse(txt, &a);
    CHECK(t == ACT_FINAL, "expected ACT_FINAL, got %d", t);
    CHECK(a.answer && !strcmp(a.answer, "all done"), "answer mismatch");
    CHECK(agent_action_validate(&a, TOOLS) == 1, "final should validate");
    agent_action_free(&a);
}

static void test_parse_ask_user(void)
{
    AgentAction a;
    const char *txt = "{\"action\":\"ask_user\",\"question\":\"Which file?\"}";
    ActionType t = agent_action_parse(txt, &a);
    CHECK(t == ACT_ASK_USER, "expected ACT_ASK_USER, got %d", t);
    CHECK(a.question && !strcmp(a.question, "Which file?"), "question mismatch");
    CHECK(agent_action_validate(&a, TOOLS) == 1, "ask_user should validate");
    agent_action_free(&a);
}

static void test_parse_plan(void)
{
    AgentAction a;
    const char *txt =
        "{\"action\":\"plan\",\"steps\":["
        "{\"id\":1,\"description\":\"do x\",\"tool\":\"\"}]}";
    ActionType t = agent_action_parse(txt, &a);
    CHECK(t == ACT_PLAN, "expected ACT_PLAN, got %d", t);
    CHECK(a.steps != NULL, "steps missing");
    CHECK(agent_action_validate(&a, TOOLS) == 1, "plan should validate: %s", a.error);
    agent_action_free(&a);
}

static void test_parse_reflect(void)
{
    AgentAction a;
    const char *txt = "{\"action\":\"reflect\",\"message\":\"thinking...\"}";
    ActionType t = agent_action_parse(txt, &a);
    CHECK(t == ACT_REFLECT, "expected ACT_REFLECT, got %d", t);
    CHECK(a.message && !strcmp(a.message, "thinking..."), "message mismatch");
    CHECK(agent_action_validate(&a, TOOLS) == 1, "reflect should validate");
    agent_action_free(&a);
}

static void test_unknown_tool_rejected(void)
{
    AgentAction a;
    const char *txt = "{\"action\":\"tool\",\"tool\":\"delete_everything\","
                      "\"arguments\":{}}";
    ActionType t = agent_action_parse(txt, &a);
    CHECK(t == ACT_TOOL, "should still parse as ACT_TOOL");
    int valid = agent_action_validate(&a, TOOLS);
    CHECK(valid == 0, "unknown tool should fail validation");
    CHECK(a.error[0] != 0, "error message should be set");
    agent_action_free(&a);
}

static void test_malformed_no_json(void)
{
    AgentAction a;
    const char *txt = "I think I should read the file but I'm not sure how to say it.";
    ActionType t = agent_action_parse(txt, &a);
    CHECK(t == ACT_INVALID, "expected ACT_INVALID for non-JSON text, got %d", t);
    CHECK(a.error[0] != 0, "error should be set for invalid parse");
    agent_action_free(&a);
}

static void test_input_alias_for_arguments(void)
{
    AgentAction a;
    const char *txt = "{\"action\":\"tool\",\"tool\":\"read_file\","
                      "\"input\":{\"path\":\"c.c\"}}";
    ActionType t = agent_action_parse(txt, &a);
    CHECK(t == ACT_TOOL, "expected ACT_TOOL with 'input' alias, got %d", t);
    CHECK(a.arguments != NULL, "arguments should be filled from 'input'");
    if (a.arguments) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(a.arguments, "path");
        CHECK(p && cJSON_IsString(p) && !strcmp(p->valuestring, "c.c"),
              "path from 'input' alias not preserved");
    }
    CHECK(agent_action_validate(&a, TOOLS) == 1, "input-alias tool action should validate");
    agent_action_free(&a);
}

int main(void)
{
    test_parse_tool_clean();
    test_parse_tool_in_prose();
    test_parse_final();
    test_parse_ask_user();
    test_parse_plan();
    test_parse_reflect();
    test_unknown_tool_rejected();
    test_malformed_no_json();
    test_input_alias_for_arguments();
    return test_report("test_protocol");
}
