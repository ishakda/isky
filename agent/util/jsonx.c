/* SPDX-License-Identifier: Apache-2.0 */
#include "jsonx.h"
#include "buf.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Scan for the first balanced top-level {...} respecting strings and escapes. */
static const char *find_object(const char *text, const char **end_out)
{
    const char *p = text;
    while (*p && *p != '{') p++;
    if (!*p) return NULL;
    const char *start = p;
    int depth = 0, in_str = 0, esc = 0;
    for (; *p; p++) {
        char c = *p;
        if (in_str) {
            if (esc) esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"') in_str = 0;
            continue;
        }
        if (c == '"') in_str = 1;
        else if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) { *end_out = p + 1; return start; }
        }
    }
    return NULL;
}

cJSON *jx_extract_object(const char *text, const char **out_start, const char **out_end)
{
    if (!text) return NULL;
    const char *p = text;
    while (*p) {
        const char *end = NULL;
        const char *start = find_object(p, &end);
        if (!start) return NULL;
        cJSON *j = cJSON_ParseWithLength(start, (size_t)(end - start));
        if (j) {
            if (out_start) *out_start = start;
            if (out_end) *out_end = end;
            return j;
        }
        p = start + 1;   /* malformed candidate: keep scanning */
    }
    return NULL;
}

/* ---- repair ---------------------------------------------------------------- */

cJSON *jx_repair_object(const char *text)
{
    if (!text) return NULL;
    const char *end = NULL;
    const char *start = find_object(text, &end);
    if (!start) return NULL;

    ABuf out;
    if (ab_init(&out) != 0) return NULL;

    const char *p = start;
    int in_str = 0, esc = 0;
    char quote = 0;
    while (p < end) {
        char c = *p;
        if (in_str) {
            if (esc) { ab_putc(&out, c); esc = 0; p++; continue; }
            if (c == '\\') { ab_putc(&out, c); esc = 1; p++; continue; }
            if (c == quote) {
                ab_putc(&out, '"');
                in_str = 0; p++;
                continue;
            }
            if (c == '"' && quote == '\'') { ab_puts(&out, "\\\""); p++; continue; }
            /* literal newline inside a string: escape it */
            if (c == '\n') { ab_puts(&out, "\\n"); p++; continue; }
            if (c == '\r') { p++; continue; }
            if (c == '\t') { ab_puts(&out, "\\t"); p++; continue; }
            ab_putc(&out, c); p++;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c; in_str = 1;
            ab_putc(&out, '"');
            p++;
            continue;
        }
        /* trailing comma before } or ] */
        if (c == ',') {
            const char *q = p + 1;
            while (q < end && isspace((unsigned char)*q)) q++;
            if (q < end && (*q == '}' || *q == ']')) { p++; continue; }
            ab_putc(&out, c); p++;
            continue;
        }
        /* Python literals */
        if (!strncmp(p, "True", 4) && !isalnum((unsigned char)p[4])) {
            ab_puts(&out, "true"); p += 4; continue;
        }
        if (!strncmp(p, "False", 5) && !isalnum((unsigned char)p[5])) {
            ab_puts(&out, "false"); p += 5; continue;
        }
        if (!strncmp(p, "None", 4) && !isalnum((unsigned char)p[4])) {
            ab_puts(&out, "null"); p += 4; continue;
        }
        /* unquoted key: identifier followed by ':' after { or , */
        if ((isalpha((unsigned char)c) || c == '_')) {
            /* look back for structural char */
            size_t ol = out.len;
            int structural = 0;
            for (size_t i = ol; i > 0; i--) {
                char b = out.data[i - 1];
                if (isspace((unsigned char)b)) continue;
                structural = (b == '{' || b == ',');
                break;
            }
            const char *q = p;
            while (q < end && (isalnum((unsigned char)*q) || *q == '_')) q++;
            const char *r = q;
            while (r < end && isspace((unsigned char)*r)) r++;
            if (structural && r < end && *r == ':') {
                ab_putc(&out, '"');
                ab_append(&out, p, (size_t)(q - p));
                ab_putc(&out, '"');
                p = q;
                continue;
            }
        }
        ab_putc(&out, c); p++;
    }

    cJSON *j = cJSON_Parse(out.data);
    ab_free(&out);
    return j;
}

/* ---- accessors ------------------------------------------------------------- */

const char *jx_str(const cJSON *obj, const char *key, const char *fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : fallback;
}

double jx_num(const cJSON *obj, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

int jx_int(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : fallback;
}

int jx_bool(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? 1 : 0;
    return fallback;
}

cJSON *jx_obj(const cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsObject(v) ? v : NULL;
}

cJSON *jx_arr(const cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsArray(v) ? v : NULL;
}

char *jx_strdup(const cJSON *obj, const char *key)
{
    const char *s = jx_str(obj, key, NULL);
    return s ? strdup(s) : NULL;
}
