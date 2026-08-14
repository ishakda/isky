/* SPDX-License-Identifier: Apache-2.0 */
/* buf.h - growable byte/string buffer used across the agent runtime.
 * Always NUL-terminated, so .data is safe to pass to any C string API. */
#ifndef AGENT_BUF_H
#define AGENT_BUF_H

#include <stddef.h>

typedef struct {
    char  *data;   /* NUL-terminated; never NULL after ab_init */
    size_t len;    /* bytes used, excluding the NUL */
    size_t cap;
} ABuf;

int   ab_init(ABuf *b);
void  ab_free(ABuf *b);
void  ab_clear(ABuf *b);
int   ab_reserve(ABuf *b, size_t extra);
int   ab_append(ABuf *b, const char *s, size_t n);
int   ab_puts(ABuf *b, const char *s);
int   ab_putc(ABuf *b, char c);
int   ab_printf(ABuf *b, const char *fmt, ...);
/* Detach the underlying string; caller frees. Buffer is reset to empty. */
char *ab_take(ABuf *b);

/* Read an entire file. Returns malloc'd NUL-terminated buffer or NULL.
 * Caps at max_bytes when max_bytes > 0 (sets *truncated when provided). */
char *read_entire_file(const char *path, size_t max_bytes, size_t *out_len, int *truncated);
/* Write buffer to file atomically-ish (write temp, rename). 0 on success. */
int   write_entire_file(const char *path, const char *data, size_t len);

#endif
