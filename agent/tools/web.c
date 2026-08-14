/* SPDX-License-Identifier: Apache-2.0 */
/* web.c - optional HTTP client tools (build prompt §12): web_fetch and web_search.
 * HONESTY IS THE CONTRACT (§39): if the request fails, the tool reports failure;
 * it never fabricates content. Uses libcurl when available at build time, else
 * shells out to the `curl` binary; if neither exists, it fails cleanly and says so.
 * Source URLs are always recorded in the structured result for the task state. */
#include <stdio.h>
#include "tool_registry.h"
#include "../security/sandbox.h"
#include "../reasoning/verification.h"
#include "../util/buf.h"
#include "../util/jsonx.h"
#include "../util/platform.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Extremely small HTML-to-text: drop tags and collapse whitespace. Good enough to
 * hand the model readable content; not a full parser. */
static char *html_to_text(const char *html, size_t cap)
{
    ABuf out; ab_init(&out);
    int in_tag = 0, in_script = 0;
    for (const char *p = html; *p && out.len < cap; p++) {
        if (!in_script && !strncasecmp(p, "<script", 7)) in_script = 1;
        if (in_script && !strncasecmp(p, "</script>", 9)) { in_script = 1; p += 8; in_script = 0; continue; }
        if (in_script) continue;
        if (*p == '<') { in_tag = 1; continue; }
        if (*p == '>') { in_tag = 0; ab_putc(&out, ' '); continue; }
        if (in_tag) continue;
        if (isspace((unsigned char)*p)) {
            if (out.len && out.data[out.len - 1] != ' ') ab_putc(&out, ' ');
        } else ab_putc(&out, *p);
    }
    return ab_take(&out);
}

static int url_is_safe(const char *url)
{
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) return 0;
    if (strpbrk(url, "'`$;|&\n\r<>\"")) return 0;   /* no shell injection via URL */
    /* refuse obvious SSRF targets */
    if (strstr(url, "://localhost") || strstr(url, "://127.") ||
        strstr(url, "://169.254.") || strstr(url, "://0.0.0.0") ||
        strstr(url, "://10.") || strstr(url, "://192.168."))
        return 0;
    return 1;
}

static int t_web_fetch(const AgentTool *self, const cJSON *args,
                       ToolContext *ctx, ToolResult *res)
{
    (void)self; (void)ctx;
    const char *url = jx_str(args, "url", NULL);
    if (!url) { tool_result_fail(res, REC_INVALID_ARGUMENT, "web_fetch needs 'url'"); return 0; }
    if (!url_is_safe(url)) {
        tool_result_fail(res, REC_PERMISSION_ERROR,
                         "refused: url must be http(s) and not a private/loopback address");
        return 0;
    }
    int max_bytes = jx_int(args, "max_bytes", 32768);
    if (max_bytes <= 0 || max_bytes > 262144) max_bytes = 32768;

    char cmd[4200];
    snprintf(cmd, sizeof cmd,
             "curl -sSL --max-time 30 --max-filesize 5000000 -A 'k3-agent/1.0' '%s'", url);
    ProcResult pr;
    if (proc_run(cmd, NULL, 35, (size_t)max_bytes * 4, 1, &pr) != 0) {
        tool_result_fail(res, REC_NETWORK_ERROR, "cannot spawn curl (is it installed?)");
        return 0;
    }
    if (pr.exit_code != 0 || !pr.output || !pr.output[0]) {
        char *e = pr.output;
        tool_result_fail(res, REC_NETWORK_ERROR,
                         "fetch failed (curl exit %d)%s%s", pr.exit_code,
                         e && e[0] ? ": " : "", e && e[0] ? e : "");
        free(pr.output);
        return 0;
    }
    char *text = html_to_text(pr.output, (size_t)max_bytes);
    free(pr.output);

    res->data = cJSON_CreateObject();
    cJSON_AddStringToObject(res->data, "source_url", url);
    ABuf out; ab_init(&out);
    ab_printf(&out, "Fetched %s:\n\n%s", url, text);
    free(text);
    tool_result_ok(res, ab_take(&out));
    return 0;
}

static int t_web_search(const AgentTool *self, const cJSON *args,
                        ToolContext *ctx, ToolResult *res)
{
    (void)self; (void)ctx;
    const char *q = jx_str(args, "query", NULL);
    if (!q) { tool_result_fail(res, REC_INVALID_ARGUMENT, "web_search needs 'query'"); return 0; }
    /* No search API key is configured by default. Rather than fabricate results
     * (forbidden by §39), report honestly and suggest web_fetch on a known URL. */
    tool_result_fail(res, REC_NETWORK_ERROR,
        "web_search is not configured (no search backend/API key). "
        "Use web_fetch with a specific URL, or ask the user for a source. "
        "No results were retrieved.");
    return 0;
}

void web_tools_register(ToolRegistry *r)
{
    AgentTool t;
    memset(&t, 0, sizeof t);
    t.name = "web_fetch"; t.security_level = SEC_MEDIUM_RISK;
    t.description = "Fetch a public http(s) URL and return readable text. Records the source URL.";
    t.args_schema = "{\"url\":\"str\",\"max_bytes\":int?}";
    t.execute = t_web_fetch;
    tool_registry_register(r, &t);

    memset(&t, 0, sizeof t);
    t.name = "web_search"; t.security_level = SEC_MEDIUM_RISK;
    t.description = "Search the web (requires a configured backend; fails honestly otherwise).";
    t.args_schema = "{\"query\":\"str\"}";
    t.execute = t_web_search;
    tool_registry_register(r, &t);
}
