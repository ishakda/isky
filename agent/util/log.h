/* SPDX-License-Identifier: Apache-2.0 */
/* log.h - structured logging for the agent runtime.
 *
 * Secrets are redacted at the sink: any message that matches common credential
 * patterns (api_key=..., Bearer ..., AWS keys, etc.) has the value replaced by
 * [REDACTED] before it is written anywhere. Callers should still avoid logging
 * secrets, but the sink is the backstop. */
#ifndef AGENT_LOG_H
#define AGENT_LOG_H

typedef enum {
    LOG_TRACE = 0,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} LogLevel;

void log_set_level(LogLevel lvl);
LogLevel log_get_level(void);
/* Optional log file (appended). NULL disables. Stderr always receives >= WARN;
 * stdout receives >= configured level when quiet mode is off. */
int  log_set_file(const char *path);
void log_set_quiet(int quiet);   /* quiet: only the file sink gets sub-WARN lines */

void log_msg(LogLevel lvl, const char *component, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
;

#define LOG_T(c, ...) log_msg(LOG_TRACE, c, __VA_ARGS__)
#define LOG_D(c, ...) log_msg(LOG_DEBUG, c, __VA_ARGS__)
#define LOG_I(c, ...) log_msg(LOG_INFO,  c, __VA_ARGS__)
#define LOG_W(c, ...) log_msg(LOG_WARN,  c, __VA_ARGS__)
#define LOG_E(c, ...) log_msg(LOG_ERROR, c, __VA_ARGS__)
#define LOG_F(c, ...) log_msg(LOG_FATAL, c, __VA_ARGS__)

/* Redact credential-looking substrings in-place-ish: returns a malloc'd copy
 * with secrets replaced. Exposed for tests. */
char *log_redact(const char *s);

#endif
