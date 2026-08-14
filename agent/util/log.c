/* SPDX-License-Identifier: Apache-2.0 */
#include "log.h"
#include "buf.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static LogLevel g_level = LOG_INFO;
static FILE    *g_file  = NULL;
static int      g_quiet = 0;

void log_set_level(LogLevel lvl) { g_level = lvl; }
LogLevel log_get_level(void)     { return g_level; }
void log_set_quiet(int quiet)    { g_quiet = quiet; }

int log_set_file(const char *path)
{
    if (g_file) { fclose(g_file); g_file = NULL; }
    if (!path) return 0;
    g_file = fopen(path, "a");
    return g_file ? 0 : -1;
}

static const char *lvl_name(LogLevel l)
{
    switch (l) {
    case LOG_TRACE: return "TRACE";
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
    default:        return "FATAL";
    }
}

/* ---- redaction -------------------------------------------------------------
 * Two pattern families:
 *   1. key=value / key: value / "key": "value" where key contains a credential
 *      word (key, token, secret, password, passwd, credential, authorization).
 *   2. bare high-entropy prefixes: "Bearer <tok>", sk-..., ghp_..., AKIA... */
static int is_cred_key(const char *k, size_t n)
{
    static const char *words[] = { "key", "token", "secret", "password",
                                   "passwd", "credential", "authorization", "auth" };
    char low[64];
    if (n >= sizeof low) n = sizeof low - 1;
    for (size_t i = 0; i < n; i++) low[i] = (char)tolower((unsigned char)k[i]);
    low[n] = 0;
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
        if (strstr(low, words[i])) return 1;
    return 0;
}

static int is_value_char(char c)
{
    return c && !isspace((unsigned char)c) && c != '"' && c != '\'' &&
           c != ',' && c != '}' && c != ']' && c != '&';
}

char *log_redact(const char *s)
{
    ABuf out;
    if (ab_init(&out) != 0) return NULL;
    size_t n = strlen(s);
    size_t i = 0;
    while (i < n) {
        /* Bearer tokens */
        if (!strncasecmp(s + i, "bearer ", 7)) {
            ab_puts(&out, "Bearer [REDACTED]");
            i += 7;
            while (i < n && is_value_char(s[i])) i++;
            continue;
        }
        /* well-known token prefixes */
        if ((!strncmp(s + i, "sk-", 3) && i + 8 < n) ||
            (!strncmp(s + i, "ghp_", 4)) || (!strncmp(s + i, "gho_", 4)) ||
            (!strncmp(s + i, "xoxb-", 5)) || (!strncmp(s + i, "xoxp-", 5)) ||
            (!strncmp(s + i, "AKIA", 4) && i + 16 < n)) {
            ab_puts(&out, "[REDACTED]");
            while (i < n && is_value_char(s[i])) i++;
            continue;
        }
        /* key = value forms */
        if (isalpha((unsigned char)s[i]) || s[i] == '_' || s[i] == '"') {
            size_t ks = i;
            if (s[i] == '"') ks = ++i;
            size_t ke = ks;
            while (ke < n && (isalnum((unsigned char)s[ke]) || s[ke] == '_' || s[ke] == '-'))
                ke++;
            size_t j = ke;
            if (j < n && s[j] == '"') j++;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
            if (j < n && (s[j] == '=' || s[j] == ':') && ke > ks &&
                is_cred_key(s + ks, ke - ks)) {
                j++;
                while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
                int quoted = (j < n && (s[j] == '"' || s[j] == '\''));
                char q = quoted ? s[j] : 0;
                if (quoted) j++;
                size_t vs = j;
                if (quoted) { while (j < n && s[j] != q) j++; }
                else {
                    while (j < n && is_value_char(s[j])) j++;
                    /* "authorization: Bearer <tok>": the value's first token is the
                     * scheme word; consume the following token (the actual secret)
                     * so it is redacted too, not left in the clear. */
                    if (j - vs == 6 && !strncasecmp(s + vs, "bearer", 6)) {
                        size_t k = j;
                        while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
                        while (k < n && is_value_char(s[k])) k++;
                        j = k;
                    }
                }
                if (j > vs) {
                    ab_append(&out, s + (s[ks - 1] == '"' && ks > 0 ? ks - 1 : ks),
                              (quoted ? vs - (ks - (ks > 0 && s[ks-1]=='"' ? 1 : 0))
                                      : vs - ks));
                    ab_puts(&out, "[REDACTED]");
                    if (quoted && j < n) { /* keep closing quote */ }
                    i = j;
                    continue;
                }
            }
            /* not a credential key: copy the identifier run */
            if (ke > i) { ab_append(&out, s + i, ke - i); i = ke; continue; }
        }
        ab_putc(&out, s[i++]);
    }
    return ab_take(&out);
}

void log_msg(LogLevel lvl, const char *component, const char *fmt, ...)
{
    if (lvl < g_level && !g_file) return;

    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    char *safe = log_redact(msg);
    const char *text = safe ? safe : msg;

    char ts[32];
    time_t t = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tmv);

    if (g_file) {
        fprintf(g_file, "%s [%s] %s: %s\n", ts, lvl_name(lvl), component, text);
        fflush(g_file);
    }
    if (lvl >= g_level) {
        FILE *sink = (lvl >= LOG_WARN) ? stderr : stdout;
        if (lvl >= LOG_WARN || !g_quiet) {
            fprintf(sink, "[%s] %s: %s\n", lvl_name(lvl), component, text);
            fflush(sink);
        }
    }
    free(safe);
}
