/* SPDX-License-Identifier: Apache-2.0 */
#include "tool.h"
#include "../reasoning/verification.h"   /* RecoveryClass values for error_class */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *security_level_name(SecurityLevel l)
{
    switch (l) {
    case SEC_SAFE:        return "SAFE";
    case SEC_LOW_RISK:    return "LOW_RISK";
    case SEC_MEDIUM_RISK: return "MEDIUM_RISK";
    case SEC_HIGH_RISK:   return "HIGH_RISK";
    case SEC_CRITICAL:    return "CRITICAL";
    }
    return "?";
}

void tool_result_init(ToolResult *r) { memset(r, 0, sizeof *r); }

void tool_result_free(ToolResult *r)
{
    if (!r) return;
    free(r->output);
    free(r->error);
    if (r->data) cJSON_Delete(r->data);
    memset(r, 0, sizeof *r);
}

void tool_result_ok(ToolResult *r, char *output_owned)
{
    r->ok = 1;
    r->output = output_owned ? output_owned : strdup("");
    r->error = NULL;
    r->error_class = 0;
}

static char *vfmt(const char *fmt, va_list ap)
{
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (n < 0) return strdup("");
    char *s = (char *)malloc((size_t)n + 1);
    if (!s) return NULL;
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    return s;
}

void tool_result_okf(ToolResult *r, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    char *s = vfmt(fmt, ap);
    va_end(ap);
    r->ok = 1;
    r->output = s ? s : strdup("");
    r->error = NULL;
}

void tool_result_fail(ToolResult *r, int error_class, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    char *s = vfmt(fmt, ap);
    va_end(ap);
    r->ok = 0;
    r->error = s ? s : strdup("error");
    r->error_class = error_class;
    if (!r->output) r->output = strdup("");
}
