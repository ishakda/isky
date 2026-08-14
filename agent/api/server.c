/* SPDX-License-Identifier: Apache-2.0 */
/* server.c - minimal loopback HTTP/1.1 API. Single-threaded; each request runs a
 * task synchronously (adequate for a local control plane). Not exposed off-host. */
#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include "../core/agent_loop.h"
#include "../util/buf.h"
#include "../util/log.h"
#include "../util/jsonx.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
/* Windows sockets are stubbed: the API is a POSIX-first feature. */
struct ApiServer { Agent *a; int port; };
ApiServer *api_server_create(Agent *a, int port) { (void)a;(void)port; return NULL; }
void api_server_destroy(ApiServer *s) { (void)s; }
int api_server_run(ApiServer *s) { (void)s; return -1; }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

struct ApiServer { Agent *a; int port; int fd; };

ApiServer *api_server_create(Agent *a, int port)
{
    ApiServer *s = (ApiServer *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->a = a; s->port = port;
    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->fd < 0) { free(s); return NULL; }
    int one = 1;
    setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* loopback only */
    addr.sin_port = htons((unsigned short)port);
    if (bind(s->fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(s->fd, 8) != 0) {
        close(s->fd); free(s); return NULL;
    }
    return s;
}

void api_server_destroy(ApiServer *s)
{
    if (!s) return;
    if (s->fd >= 0) close(s->fd);
    free(s);
}

static void send_json(int c, int code, const char *status, const char *body)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        code, status, strlen(body));
    if (write(c, hdr, (size_t)n) < 0) return;
    if (write(c, body, strlen(body)) < 0) return;
}

/* very small request parse: method, path, and body after \r\n\r\n */
static int handle(ApiServer *s, const char *method, const char *path,
                  const char *body, ABuf *out)
{
    Agent *a = s->a;
    if (!strcmp(method, "GET") && !strcmp(path, "/v1/tools")) {
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < tool_registry_count(a->tools); i++) {
            const AgentTool *t = tool_registry_at(a->tools, i);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", t->name);
            cJSON_AddStringToObject(o, "description", t->description);
            cJSON_AddStringToObject(o, "security_level",
                                    security_level_name(t->security_level));
            cJSON_AddItemToArray(arr, o);
        }
        char *js = cJSON_PrintUnformatted(arr);
        ab_puts(out, js);
        free(js); cJSON_Delete(arr);
        return 200;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/v1/memory")) {
        cJSON *arr = a->db ? db_search_memories(a->db, NULL, NULL, 50)
                           : cJSON_CreateArray();
        char *js = cJSON_PrintUnformatted(arr);
        ab_puts(out, js); free(js); cJSON_Delete(arr);
        return 200;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/v1/tasks")) {
        cJSON *req = body ? cJSON_Parse(body) : NULL;
        const char *goal = req ? jx_str(req, "goal", NULL) : NULL;
        if (!goal) { ab_puts(out, "{\"error\":\"goal required\"}"); if (req) cJSON_Delete(req); return 400; }
        AgentTask *t = task_create(goal, a->cfg.max_iterations);
        RunOptions opt; memset(&opt, 0, sizeof opt);
        RunOutcome oc = agent_loop_run(a, t, &opt);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "task_id", t->id);
        cJSON_AddStringToObject(o, "status",
            oc == RUN_COMPLETED ? "completed" :
            oc == RUN_NEEDS_USER ? "waiting_user" : "failed");
        if (t->final_answer) cJSON_AddStringToObject(o, "answer", t->final_answer);
        if (t->user_question) cJSON_AddStringToObject(o, "question", t->user_question);
        char *js = cJSON_PrintUnformatted(o);
        ab_puts(out, js); free(js); cJSON_Delete(o);
        task_free(t);
        if (req) cJSON_Delete(req);
        return 200;
    }
    /* POST /v1/tasks/:id/cancel  and  POST /v1/tasks/:id/resume */
    if (!strcmp(method, "POST") && !strncmp(path, "/v1/tasks/", 10)) {
        char idbuf[64]; snprintf(idbuf, sizeof idbuf, "%s", path + 10);
        char *slash = strchr(idbuf, '/');
        const char *verb = slash ? slash + 1 : "";
        if (slash) *slash = 0;
        if (!a->db) { ab_puts(out, "{\"error\":\"database disabled\"}"); return 400; }
        AgentTask t; memset(&t, 0, sizeof t);
        if (db_load_task(a->db, idbuf, &t) != 1) { ab_puts(out, "{\"error\":\"not found\"}"); return 404; }
        int code = 200;
        if (!strcmp(verb, "cancel")) {
            t.state = AGENT_FAILED;
            task_set_answer(&t, "Task cancelled via API.");
            db_save_task(a->db, &t);
            ab_puts(out, "{\"status\":\"cancelled\"}");
        } else if (!strcmp(verb, "resume")) {
            cJSON *req = body ? cJSON_Parse(body) : NULL;
            const char *ans = req ? jx_str(req, "answer", "(resumed)") : "(resumed)";
            RunOptions opt; memset(&opt, 0, sizeof opt);
            RunOutcome oc = agent_loop_resume(a, &t, ans, &opt);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "task_id", t.id);
            cJSON_AddStringToObject(o, "status",
                oc == RUN_COMPLETED ? "completed" :
                oc == RUN_NEEDS_USER ? "waiting_user" : "failed");
            if (t.final_answer) cJSON_AddStringToObject(o, "answer", t.final_answer);
            char *js = cJSON_PrintUnformatted(o); ab_puts(out, js); free(js);
            cJSON_Delete(o); if (req) cJSON_Delete(req);
        } else {
            ab_puts(out, "{\"error\":\"unknown task action\"}"); code = 404;
        }
        free(t.goal); free(t.plan); free(t.user_question); free(t.final_answer);
        return code;
    }
    /* GET /v1/tasks/:id/events -> the persisted execution trace */
    if (!strcmp(method, "GET") && !strncmp(path, "/v1/tasks/", 10) &&
        strstr(path, "/events")) {
        char idbuf[64]; snprintf(idbuf, sizeof idbuf, "%s", path + 10);
        char *slash = strchr(idbuf, '/'); if (slash) *slash = 0;
        cJSON *tr = a->db ? db_get_trace(a->db, idbuf) : cJSON_CreateArray();
        char *js = cJSON_PrintUnformatted(tr); ab_puts(out, js); free(js);
        cJSON_Delete(tr);
        return 200;
    }
    if (!strcmp(method, "GET") && !strncmp(path, "/v1/tasks/", 10)) {
        const char *id = path + 10;
        char idbuf[64]; snprintf(idbuf, sizeof idbuf, "%s", id);
        char *slash = strchr(idbuf, '/'); if (slash) *slash = 0;
        AgentTask t; memset(&t, 0, sizeof t);
        int found = a->db ? db_load_task(a->db, idbuf, &t) : 0;
        if (found != 1) { ab_puts(out, "{\"error\":\"not found\"}"); return 404; }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "task_id", t.id);
        cJSON_AddStringToObject(o, "state", agent_state_name(t.state));
        cJSON_AddNumberToObject(o, "success", t.success);
        if (t.final_answer) cJSON_AddStringToObject(o, "answer", t.final_answer);
        cJSON *tr = db_get_trace(a->db, idbuf);
        cJSON_AddItemToObject(o, "trace", tr);
        char *js = cJSON_PrintUnformatted(o);
        ab_puts(out, js); free(js); cJSON_Delete(o);
        free(t.goal); free(t.plan); free(t.user_question); free(t.final_answer);
        return 200;
    }
    ab_puts(out, "{\"error\":\"not found\"}");
    return 404;
}

int api_server_run(ApiServer *s)
{
    LOG_I("api", "listening on http://127.0.0.1:%d", s->port);
    for (;;) {
        int c = accept(s->fd, NULL, NULL);
        if (c < 0) continue;
        char buf[65536];
        ssize_t n = read(c, buf, sizeof buf - 1);
        if (n <= 0) { close(c); continue; }
        buf[n] = 0;
        char method[8] = {0}, path[1024] = {0};
        sscanf(buf, "%7s %1023s", method, path);
        const char *body = strstr(buf, "\r\n\r\n");
        body = body ? body + 4 : NULL;
        ABuf out; ab_init(&out);
        int code = handle(s, method, path, body, &out);
        send_json(c, code, code == 200 ? "OK" : code == 404 ? "Not Found" : "Bad Request",
                  out.data);
        ab_free(&out);
        close(c);
    }
    return 0;
}
#endif
