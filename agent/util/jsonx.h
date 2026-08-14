/* SPDX-License-Identifier: Apache-2.0 */
/* jsonx.h - JSON helpers over cJSON: extraction of a JSON object from raw model
 * output, light repair, and typed field accessors used by protocol validation. */
#ifndef AGENT_JSONX_H
#define AGENT_JSONX_H

#include "cJSON.h"

/* Extract the first complete top-level JSON object from `text`, which may be
 * surrounded by prose or markdown fences (```json ... ```). Balanced-brace scan
 * that respects strings and escapes. Returns parsed cJSON or NULL.
 * When out_start/out_end are non-NULL they receive the byte span used. */
cJSON *jx_extract_object(const char *text, const char **out_start, const char **out_end);

/* Attempt structured repair of ALMOST-JSON: trailing commas, single quotes on
 * keys/strings, unquoted keys, Python literals (True/False/None). Returns parsed
 * cJSON or NULL. Never executes anything; this is text munging + strict re-parse. */
cJSON *jx_repair_object(const char *text);

/* Typed accessors: return value or fallback; never crash on wrong types. */
const char *jx_str(const cJSON *obj, const char *key, const char *fallback);
double      jx_num(const cJSON *obj, const char *key, double fallback);
int         jx_int(const cJSON *obj, const char *key, int fallback);
int         jx_bool(const cJSON *obj, const char *key, int fallback);
cJSON      *jx_obj(const cJSON *obj, const char *key);   /* object or NULL */
cJSON      *jx_arr(const cJSON *obj, const char *key);   /* array or NULL */

/* strdup of a string field, or NULL. Caller frees. */
char *jx_strdup(const cJSON *obj, const char *key);

#endif
