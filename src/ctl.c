#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

static const char *g_sock = DEFAULT_SOCK_PATH;

static void usage(const char *prog)
{
    printf(
        "用法: %s [-s socket] <命令>\n"
        "\n"
        "命令:\n"
        "  get                     获取全部配置与当前状态\n"
        "  set <key> <value>       修改一项配置\n"
        "  high <℃>                设置高阈值 (等同 set high)\n"
        "  low  <℃>                设置低阈值\n"
        "  interval <ms>           设置轮询间隔\n"
        "  step <值>               设置步长\n"
        "  night <on|off>          夜间模式开关\n"
        "  night-start <HH:MM>     夜间起始\n"
        "  night-end   <HH:MM>     夜间结束\n"
        "\n"
        "选项:\n"
        "  -s, --socket <path>     控制 socket 路径 (默认 %s)\n"
        "  -h, --help              显示本帮助\n"
        "\n"
        "示例:\n"
        "  %s get\n"
        "  %s high 85\n"
        "  %s set low 70\n"
        "  %s night on\n"
        "  %s night-start 23:30\n",
        prog, DEFAULT_SOCK_PATH, prog, prog, prog, prog, prog);
}

/* 连接守护进程，发送 req，把响应打印到 stdout。返回 0 成功。 */
static int send_request(const char *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket() 失败: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(g_sock) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "socket 路径过长\n");
        close(fd);
        return 1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_sock);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "无法连接 %s: %s\n", g_sock, strerror(errno));
        fprintf(stderr, "(守护进程未运行？或 socket 路径不对，用 -s 指定)\n");
        close(fd);
        return 1;
    }

    size_t len = strlen(req), off = 0;
    while (off < len) {
        ssize_t w = write(fd, req + off, len - off);
        if (w <= 0) {
            fprintf(stderr, "写请求失败: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
        off += (size_t)w;
    }

    char buf[2048];
    ssize_t n;
    int saw_err = 0;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        if (strstr(buf, "ERR "))
            saw_err = 1;
        fputs(buf, stdout);
    }
    close(fd);
    return saw_err ? 1 : 0;
}

int main(int argc, char **argv)
{
    int i = 1;

    /* 解析全局选项 (-s/--socket, -h/--help) */
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        }
        if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--socket")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s 需要参数\n", argv[i]);
                return 2;
            }
            g_sock = argv[i + 1];
            i += 2;
            continue;
        }
        fprintf(stderr, "未知选项: %s\n", argv[i]);
        return 2;
    }

    if (i >= argc) {
        usage(argv[0]);
        return 2;
    }

    const char *cmd = argv[i++];
    char req[256];

    if (!strcmp(cmd, "get")) {
        snprintf(req, sizeof(req), "GET\n");
    } else if (!strcmp(cmd, "set")) {
        if (i + 1 >= argc) {
            fprintf(stderr, "用法: set <key> <value>\n");
            return 2;
        }
        snprintf(req, sizeof(req), "SET %s %s\n", argv[i], argv[i + 1]);
    } else if (!strcmp(cmd, "high")  || !strcmp(cmd, "low") ||
               !strcmp(cmd, "interval") || !strcmp(cmd, "step") ||
               !strcmp(cmd, "night") || !strcmp(cmd, "night-start") ||
               !strcmp(cmd, "night-end")) {
        if (i >= argc) {
            fprintf(stderr, "用法: %s <value>\n", cmd);
            return 2;
        }
        snprintf(req, sizeof(req), "SET %s %s\n", cmd, argv[i]);
    } else {
        fprintf(stderr, "未知命令: %s\n", cmd);
        usage(argv[0]);
        return 2;
    }

    return send_request(req);
}
