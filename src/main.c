#include "config.h"
#include "thermal.h"
#include "limiter.h"
#include "control.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

typedef struct {
    int          high_c;
    int          low_c;
    int          interval_ms;
    limit_mode_t mode;
    int          step;
    long         floor;       /* freq:kHz / power:uW，<=0 表示默认 */
    int          dry_run;
    int          verbose;
    int          night_enable;     /* 是否启用夜间空闲时段 */
    int          night_start_min;  /* 夜间起始 (当天分钟) */
    int          night_end_min;    /* 夜间结束 (当天分钟) */
    const char  *sock_path;        /* 控制 socket 路径 */
    int          no_control;       /* 1: 不启动控制接口 */
} options_t;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *prog)
{
    printf(
        "用法: %s [选项]\n"
        "  -H, --high <℃>       触发限制的高阈值 (默认 %d)\n"
        "  -L, --low  <℃>       解除限制的低阈值 (默认 %d)\n"
        "  -i, --interval <ms>  轮询间隔毫秒 (默认 %d)\n"
        "  -m, --mode freq|power  限制方式 (默认 freq)\n"
        "  -s, --step <值>      调整步长 (freq:MHz / power:W)\n"
        "  -f, --floor <值>     最低上限地板 (freq:MHz / power:W, 默认自动)\n"
        "  -N, --night          启用夜间空闲时段：强制压到最低\n"
        "  --night-start HH:MM  夜间起始时间 (默认 %02d:%02d)\n"
        "  --night-end   HH:MM  夜间结束时间 (默认 %02d:%02d)\n"
        "  -S, --socket <path>  控制 socket 路径 (默认 %s)\n"
        "  -C, --no-control     不启动控制接口\n"
        "  -n, --dry-run        不实际写入，仅打印决策\n"
        "  -v, --verbose        打印每次轮询的温度与动作\n"
        "  -h, --help           显示本帮助\n",
        prog, DEFAULT_HIGH_C, DEFAULT_LOW_C, DEFAULT_INTERVAL_MS,
        DEFAULT_NIGHT_START_MIN / 60, DEFAULT_NIGHT_START_MIN % 60,
        DEFAULT_NIGHT_END_MIN / 60, DEFAULT_NIGHT_END_MIN % 60,
        DEFAULT_SOCK_PATH);
}

static int parse_hhmm(const char *s, int *out_min)
{
    int hh, mm;
    if (sscanf(s, "%d:%d", &hh, &mm) != 2)
        return -1;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59)
        return -1;
    *out_min = hh * 60 + mm;
    return 0;
}

/* 判断 now_min 是否在 [start, end) 夜间窗口内（支持跨午夜）。
 * 例如 start=0(00:00) end=360(06:00)：0..359 命中。
 * 跨午夜例：start=1320(22:00) end=360(06:00)：>=1320 或 <360 命中。 */
static int in_night_window(int now_min, int start, int end)
{
    if (start == end)
        return 0;  /* 空窗口 */
    if (start < end)
        return now_min >= start && now_min < end;
    /* 跨午夜 */
    return now_min >= start || now_min < end;
}

static int parse_args(int argc, char **argv, options_t *o)
{
    static const struct option longopts[] = {
        {"high",        required_argument, 0, 'H'},
        {"low",         required_argument, 0, 'L'},
        {"interval",    required_argument, 0, 'i'},
        {"mode",        required_argument, 0, 'm'},
        {"step",        required_argument, 0, 's'},
        {"floor",       required_argument, 0, 'f'},
        {"night",       no_argument,       0, 'N'},
        {"night-start", required_argument, 0, 1001},
        {"night-end",   required_argument, 0, 1002},
        {"socket",      required_argument, 0, 'S'},
        {"no-control",  no_argument,       0, 'C'},
        {"dry-run",     no_argument,       0, 'n'},
        {"verbose",     no_argument,       0, 'v'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    o->high_c          = DEFAULT_HIGH_C;
    o->low_c           = DEFAULT_LOW_C;
    o->interval_ms     = DEFAULT_INTERVAL_MS;
    o->mode            = LIMIT_MODE_FREQ;
    o->step            = -1;
    o->floor           = 0;
    o->dry_run         = 0;
    o->verbose         = 0;
    o->night_enable    = 0;
    o->night_start_min = DEFAULT_NIGHT_START_MIN;
    o->night_end_min   = DEFAULT_NIGHT_END_MIN;
    o->sock_path       = DEFAULT_SOCK_PATH;
    o->no_control      = 0;

    int floor_arg = -1;
    int c;
    while ((c = getopt_long(argc, argv, "H:L:i:m:s:f:NS:Cnvh", longopts, NULL)) != -1) {
        switch (c) {
        case 'H': o->high_c = atoi(optarg); break;
        case 'L': o->low_c = atoi(optarg); break;
        case 'i': o->interval_ms = atoi(optarg); break;
        case 'm':
            if (limiter_parse_mode(optarg, &o->mode) != 0) {
                fprintf(stderr, "无效的 --mode: %s (应为 freq 或 power)\n", optarg);
                return -1;
            }
            break;
        case 's': o->step = atoi(optarg); break;
        case 'f': floor_arg = atoi(optarg); break;
        case 'N': o->night_enable = 1; break;
        case 1001:
            if (parse_hhmm(optarg, &o->night_start_min) != 0) {
                fprintf(stderr, "无效的 --night-start: %s (应为 HH:MM)\n", optarg);
                return -1;
            }
            break;
        case 1002:
            if (parse_hhmm(optarg, &o->night_end_min) != 0) {
                fprintf(stderr, "无效的 --night-end: %s (应为 HH:MM)\n", optarg);
                return -1;
            }
            break;
        case 'n': o->dry_run = 1; break;
        case 'v': o->verbose = 1; break;
        case 'S': o->sock_path = optarg; break;
        case 'C': o->no_control = 1; break;
        case 'h': usage(argv[0]); exit(0);
        default:  return -1;
        }
    }

    if (o->step < 0)
        o->step = (o->mode == LIMIT_MODE_FREQ) ? DEFAULT_STEP_MHZ : DEFAULT_STEP_W;

    /* floor 单位换算: freq MHz->kHz, power W->uW */
    if (floor_arg > 0)
        o->floor = (o->mode == LIMIT_MODE_FREQ)
                       ? (long)floor_arg * 1000L
                       : (long)floor_arg * 1000000L;

    if (o->low_c >= o->high_c) {
        fprintf(stderr, "错误: 低阈值(%d) 必须小于高阈值(%d)\n", o->low_c, o->high_c);
        return -1;
    }
    if (o->interval_ms < 50) {
        fprintf(stderr, "错误: 轮询间隔过小 (%d ms)\n", o->interval_ms);
        return -1;
    }
    if (o->step <= 0) {
        fprintf(stderr, "错误: 步长必须为正 (%d)\n", o->step);
        return -1;
    }
    if (o->night_enable && o->night_start_min == o->night_end_min) {
        fprintf(stderr, "错误: 夜间起止时间不能相同\n");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    options_t opt;
    if (parse_args(argc, argv, &opt) != 0)
        return 2;

    /* 安装信号处理: 收到信号置位，主循环退出后统一恢复 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    thermal_source_t src;
    if (thermal_select(&src) != 0) {
        fprintf(stderr, "无法选定温度源，退出。\n");
        return 1;
    }

    long floor_freq = (opt.mode == LIMIT_MODE_FREQ) ? opt.floor : 0;
    long floor_pow  = (opt.mode == LIMIT_MODE_POWER) ? opt.floor : 0;

    limiter_t lm;
    int lrc = limiter_init(&lm, opt.mode, opt.dry_run, floor_freq, floor_pow);
    if (lrc < 0) {
        fprintf(stderr, "limiter 初始化失败，退出。\n");
        return 1;
    }

    /* 运行时可修改的配置 (ctl SET 直接改这里) */
    guard_config_t cfg = {
        .high_c          = opt.high_c,
        .low_c           = opt.low_c,
        .interval_ms     = opt.interval_ms,
        .step            = opt.step,
        .night_enable    = opt.night_enable,
        .night_start_min = opt.night_start_min,
        .night_end_min   = opt.night_end_min,
    };

    guard_runtime_t rt;
    memset(&rt, 0, sizeof(rt));
    rt.cfg     = &cfg;
    rt.lm      = &lm;
    rt.src     = &src;
    rt.dry_run = opt.dry_run;
    snprintf(rt.last_action, sizeof(rt.last_action), "-");

    /* 启动控制 socket (失败则退化为无控制接口) */
    int ctl_fd = -1;
    if (!opt.no_control) {
        ctl_fd = control_start(opt.sock_path);
        if (ctl_fd < 0)
            fprintf(stderr, "控制接口未启用，继续运行(仅热控)。\n");
    }

    char desc[160];
    limiter_describe(&lm, desc, sizeof(desc));

    printf("CPU Thermal Guard\n");
    printf("  温度源 : %s (%s)\n", src.type, src.temp_path);
    printf("  阈值   : 高=%d℃  低=%d℃\n", cfg.high_c, cfg.low_c);
    printf("  模式   : %s  步长=%d  间隔=%dms\n",
           limiter_mode_name(opt.mode), cfg.step, cfg.interval_ms);
    printf("  控制   : %s\n", desc);
    if (cfg.night_enable)
        printf("  夜间   : %02d:%02d-%02d:%02d 时段内强制最低\n",
               cfg.night_start_min / 60, cfg.night_start_min % 60,
               cfg.night_end_min   / 60, cfg.night_end_min   % 60);
    printf("  接口   : %s\n", ctl_fd >= 0 ? opt.sock_path : "(无)");
    printf("  状态   : %s\n", lm.active ? "可写(将实际限制)"
                                        : (opt.dry_run ? "dry-run(仅打印)"
                                                       : "只读监控(无权限)"));
    printf("  按 Ctrl-C 退出，退出时自动恢复原始设置。\n\n");

    while (!g_stop) {
        long millic;
        if (thermal_read(&src, &millic) != 0) {
            fprintf(stderr, "读取温度失败，跳过本次。\n");
            control_wait(ctl_fd, cfg.interval_ms, &rt);
            continue;
        }

        rt.last_millic = millic;
        long temp_c = millic / 1000;
        const char *action = "保持";

        /* 计算当前是否应处于夜间强制态 (随运行时 cfg 实时变化) */
        int want_night = 0;
        if (cfg.night_enable && cfg.night_start_min != cfg.night_end_min) {
            struct tm tm;
            time_t now = time(NULL);
            localtime_r(&now, &tm);
            int now_min = tm.tm_hour * 60 + tm.tm_min;
            want_night = in_night_window(now_min, cfg.night_start_min,
                                         cfg.night_end_min);
        }

        if (want_night && !rt.night_active) {
            limiter_force_floor(&lm);
            limiter_describe(&lm, desc, sizeof(desc));
            rt.night_active = 1;
            action = "进入夜间(压到最低)";
        } else if (!want_night && rt.night_active) {
            limiter_force_ceiling(&lm);
            limiter_describe(&lm, desc, sizeof(desc));
            rt.night_active = 0;
            action = "退出夜间(放开)";
        } else if (rt.night_active) {
            action = "夜间(保持最低)";
        } else {
            /* 白天/未启用夜间: 正常滞回热控 */
            if (temp_c >= cfg.high_c) {
                if (limiter_tighten(&lm, cfg.step)) {
                    limiter_describe(&lm, desc, sizeof(desc));
                    action = "收紧";
                } else {
                    action = "已至地板";
                }
            } else if (temp_c <= cfg.low_c) {
                if (limiter_is_limited(&lm)) {
                    if (limiter_relax(&lm, cfg.step)) {
                        limiter_describe(&lm, desc, sizeof(desc));
                        action = "放宽";
                    }
                }
            }
        }

        snprintf(rt.last_action, sizeof(rt.last_action), "%s", action);

        if (opt.verbose)
            printf("temp=%ld.%03ld℃  动作=%s  %s\n",
                   millic / 1000, millic % 1000, action, desc);

        control_wait(ctl_fd, cfg.interval_ms, &rt);
    }

    printf("\n收到退出信号，恢复原始设置...\n");
    control_stop(ctl_fd, opt.no_control ? NULL : opt.sock_path);
    limiter_free(&lm);
    printf("已恢复，退出。\n");
    return 0;
}
