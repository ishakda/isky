/* SPDX-License-Identifier: Apache-2.0 */
#include "buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ab_init(ABuf *b)
{
    b->cap = 64;
    b->len = 0;
    b->data = (char *)malloc(b->cap);
    if (!b->data) { b->cap = 0; return -1; }
    b->data[0] = 0;
    return 0;
}

void ab_free(ABuf *b)
{
    free(b->data);
    b->data = NULL; b->len = b->cap = 0;
}

void ab_clear(ABuf *b)
{
    b->len = 0;
    if (b->data) b->data[0] = 0;
}

int ab_reserve(ABuf *b, size_t extra)
{
    if (!b->data && ab_init(b) != 0) return -1;
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) cap *= 2;
    char *p = (char *)realloc(b->data, cap);
    if (!p) return -1;
    b->data = p; b->cap = cap;
    return 0;
}

int ab_append(ABuf *b, const char *s, size_t n)
{
    if (ab_reserve(b, n) != 0) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
    return 0;
}

int ab_puts(ABuf *b, const char *s) { return ab_append(b, s, strlen(s)); }
int ab_putc(ABuf *b, char c)        { return ab_append(b, &c, 1); }

int ab_printf(ABuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if (ab_reserve(b, (size_t)n) != 0) return -1;
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    return 0;
}

char *ab_take(ABuf *b)
{
    char *p = b->data;
    b->data = NULL; b->len = b->cap = 0;
    ab_init(b);
    return p;
}

char *read_entire_file(const char *path, size_t max_bytes, size_t *out_len, int *truncated)
{
    if (truncated) *truncated = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    size_t want = (size_t)sz;
    if (max_bytes > 0 && want > max_bytes) {
        want = max_bytes;
        if (truncated) *truncated = 1;
    }
    char *buf = (char *)malloc(want + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, want, f);
    fclose(f);
    buf[got] = 0;
    if (out_len) *out_len = got;
    return buf;
}

int write_entire_file(const char *path, const char *data, size_t len)
{
    char tmp[4096];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.agent", path);
    if (n < 0 || (size_t)n >= sizeof tmp) return -1;
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    if (fclose(f) != 0 || w != len) { remove(tmp); return -1; }
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}
