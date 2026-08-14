/* SPDX-License-Identifier: Apache-2.0 */
/* Tiny test harness shared by the agent test suite. */
#ifndef AGENT_TEST_UTIL_H
#define AGENT_TEST_UTIL_H
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_checks = 0, g_fails = 0;

#define CHECK(cond, ...) do {                                   \
    g_checks++;                                                 \
    if (!(cond)) {                                              \
        g_fails++;                                              \
        fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);  \
        fprintf(stderr, __VA_ARGS__);                           \
        fprintf(stderr, "\n");                                  \
    }                                                           \
} while (0)

#define CHECK_STR_EQ(a, b) CHECK(strcmp((a),(b))==0, "'%s' != '%s'", (a), (b))

static int test_report(const char *name)
{
    printf("%s: %d checks, %d failures\n", name, g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}

#endif
