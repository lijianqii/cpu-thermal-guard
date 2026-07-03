#include "control.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

int control_start(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "control: socket() 失败: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "control: socket 路径过长: %s\n", path);
        close(fd);
        return -1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    unlink(path); /* 清理上次残留 */

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "control: bind %s 失败: %s (控制接口将不可用)\n",
                path, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 4) != 0) {
        fprintf(stderr, "control: listen 失败: %s\n", strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    return fd;
}

void control_stop(int listen_fd, const char *path)
{
    if (listen_fd >= 0)
        close(listen_fd);
    if (path)
        unlink(path);
}



static int parse_bool(const char *s, int *out)
{
    if (!strcmp(s, "on") || !strcmp(s, "1") || !strcmp(s, "true")) {
        *out = 1; return 0;
    }
    if (!strcmp(s, "off") || !strcmp(s, "0") || !strcmp(s, "false")) {
        *out = 0; return 0;
    }
    return -1;
}

/* 组装 GET 响应。 */
static void build_status(const guard_runtime_t *rt, char *buf, size_t buflen)
{
    const guard_config_t *c = rt->cfg;
    char limit[160];
    limiter_describe(rt->lm, limit, sizeof(limit));

    const char *state = rt->lm->active ? "active"
                      : (rt->dry_run ? "dry-run" : "monitor");

    snprintf(buf, buflen,
        "high=%d\n"
        "low=%d\n"
        "interval=%d\n"
        "mode=%s\n"
        "step=%d\n"
        "night=%s\n"
        "night-start=%02d:%02d\n"
        "night-end=%02d:%02d\n"
        "night-active=%s\n"
        "state=%s\n"
        "temp=%ld.%03ld\n"
        "action=%s\n"
        "limit=%s\n"
        "OK\n",
        c->high_c, c->low_c, c->interval_ms,
        limiter_mode_name(rt->lm->mode), c->step,
        c->night_enable ? "on" : "off",
        c->night_start_min / 60, c->night_start_min % 60,
        c->night_end_min / 60, c->night_end_min % 60,
        rt->night_active ? "yes" : "no",
        state,
        rt->last_millic / 1000, (rt->last_millic % 1000 + 1000) % 1000,
        rt->last_action[0] ? rt->last_action : "-",
        limit);
}

/* 处理 SET，把结果(OK\n 或 ERR ...\n)写入 resp。 */
static void handle_set(guard_runtime_t *rt, const char *key, const char *val,
                       char *resp, size_t resplen)
{
    guard_config_t *c = rt->cfg;

    if (!strcmp(key, "high")) {
        int v = atoi(val);
        if (v <= c->low_c) { snprintf(resp, resplen, "ERR high 必须大于 low(%d)\n", c->low_c); return; }
        c->high_c = v;
    } else if (!strcmp(key, "low")) {
        int v = atoi(val);
        if (v >= c->high_c) { snprintf(resp, resplen, "ERR low 必须小于 high(%d)\n", c->high_c); return; }
        c->low_c = v;
    } else if (!strcmp(key, "interval")) {
        int v = atoi(val);
        if (v < 50) { snprintf(resp, resplen, "ERR interval 过小(>=50)\n"); return; }
        c->interval_ms = v;
    } else if (!strcmp(key, "step")) {
        int v = atoi(val);
        if (v <= 0) { snprintf(resp, resplen, "ERR step 必须为正\n"); return; }
        c->step = v;
    } else if (!strcmp(key, "night")) {
        int v;
        if (parse_bool(val, &v) != 0) { snprintf(resp, resplen, "ERR night 应为 on/off\n"); return; }
        c->night_enable = v;
    } else if (!strcmp(key, "night-start")) {
        int m;
        if (parse_hhmm(val, &m) != 0) { snprintf(resp, resplen, "ERR night-start 应为 HH:MM\n"); return; }
        c->night_start_min = m;
    } else if (!strcmp(key, "night-end")) {
        int m;
        if (parse_hhmm(val, &m) != 0) { snprintf(resp, resplen, "ERR night-end 应为 HH:MM\n"); return; }
        c->night_end_min = m;
    } else {
        snprintf(resp, resplen, "ERR 未知配置项: %s\n", key);
        return;
    }
    snprintf(resp, resplen, "OK\n");
}

static void serve_one(int cfd, guard_runtime_t *rt)
{
    char req[256];
    ssize_t n = read(cfd, req, sizeof(req) - 1);
    if (n <= 0)
        return;
    req[n] = '\0';
    req[strcspn(req, "\r\n")] = '\0';

    char resp[1024];

    if (!strcmp(req, "GET")) {
        build_status(rt, resp, sizeof(resp));
    } else if (!strncmp(req, "SET ", 4)) {
        char key[64] = {0}, val[128] = {0};
        if (sscanf(req + 4, "%63s %127s", key, val) == 2) {
            handle_set(rt, key, val, resp, sizeof(resp));
        } else {
            snprintf(resp, sizeof(resp), "ERR 用法: SET <key> <value>\n");
        }
    } else {
        snprintf(resp, sizeof(resp), "ERR 未知命令 (GET 或 SET)\n");
    }

    size_t len = strlen(resp), off = 0;
    while (off < len) {
        ssize_t w = write(cfd, resp + off, len - off);
        if (w <= 0)
            break;
        off += (size_t)w;
    }
}

void control_wait(int listen_fd, int timeout_ms, guard_runtime_t *rt)
{
    if (listen_fd < 0) {
        struct timespec ts;
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
        return;
    }

    struct timespec deadline, now;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    long add_ns = deadline.tv_nsec + (long)(timeout_ms % 1000) * 1000000L;
    deadline.tv_sec += timeout_ms / 1000 + add_ns / 1000000000L;
    deadline.tv_nsec = add_ns % 1000000000L;

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remain = (deadline.tv_sec - now.tv_sec) * 1000L +
                      (deadline.tv_nsec - now.tv_nsec) / 1000000L;
        if (remain <= 0)
            return;

        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, (int)remain);
        if (pr < 0) {
            if (errno == EINTR)
                return; /* 被信号打断: 交回主循环检查 g_stop */
            return;
        }
        if (pr == 0)
            return; /* 超时 */
        if (pfd.revents & POLLIN) {
            int cfd = accept(listen_fd, NULL, NULL);
            if (cfd >= 0) {
                serve_one(cfd, rt);
                close(cfd);
            }
        }
    }
}
