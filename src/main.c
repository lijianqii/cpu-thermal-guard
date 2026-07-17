#include "config.h"
#include "thermal.h"
#include "limiter.h"
#include "control.h"
#include "scheduler.h"
#include "protocol.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <time.h>

typedef struct {
    int          high_c;
    int          low_c;
    int          interval_ms;
    limit_mode_t mode;
    int          step;
    long         floor;
    int          dry_run;
    int          verbose;
    int          night_enable;
    int          night_start_min;
    int          night_end_min;
    int          weekend_enable;
    const char  *sock_path;
    int          no_control;
    int          warm_margin;
    int          critical_margin;
    int          cool_confirm;
    long         rise_rate;
    long         fall_rate;
    const char  *mock_temp_path;
    int          mock_limit;
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
        "  -H, --high <C>            高阈值 (默认 %d)\n"
        "  -L, --low  <C>            低阈值 (默认 %d)\n"
        "  -i, --interval <ms>        轮询间隔 (默认 %d)\n"
        "  -m, --mode freq|power      限制方式 (默认 freq)\n"
        "  -s, --step <值>            步长 (freq:MHz / power:W)\n"
        "  -f, --floor <值>           地板 (freq:MHz / power:W)\n"
        "  -N, --night                夜间强制最低\n"
        "  --night-start HH:MM        夜间起始 (默认 %02d:%02d)\n"
        "  --night-end   HH:MM        夜间结束 (默认 %02d:%02d)\n"
        "  -W, --weekend              周末全天最低\n"
        "  --warm-margin <C>          WARM 预警区偏移 (默认 %d)\n"
        "  --critical-margin <C>      CRITICAL 紧急区偏移 (默认 %d)\n"
        "  --cool-confirm <n>          COLD 确认轮数 (默认 %d)\n"
        "  --rise-rate <mC/s>          上升触发阈值 (默认 %ld)\n"
        "  --fall-rate <mC/s>          下降加速阈值 (默认 %ld)\n"
        "  -S, --socket <path>        socket 路径 (默认 %s)\n"
        "  -C, --no-control           不启动控制接口\n"
        "  -n, --dry-run              仅打印不写入\n"
        "  -v, --verbose              打印每次轮询\n"
        "  --mock-temp <path>         从文件读温度 (测试)\n"
        "  --mock-limit               内存模拟限幅器 (测试)\n"
        "  -h, --help                 帮助\n",
        prog, DEFAULT_HIGH_C, DEFAULT_LOW_C, DEFAULT_INTERVAL_MS,
        DEFAULT_NIGHT_START_MIN / 60, DEFAULT_NIGHT_START_MIN % 60,
        DEFAULT_NIGHT_END_MIN / 60, DEFAULT_NIGHT_END_MIN % 60,
        DEFAULT_WARM_MARGIN_C, DEFAULT_CRITICAL_MARGIN_C, DEFAULT_COOL_CONFIRM,
        (long)DEFAULT_RISE_RATE, (long)DEFAULT_FALL_RATE,
        DEFAULT_SOCK_PATH);
}

static int in_night_window(int now_min, int start, int end)
{
    if (start == end)
        return 0;
    if (start < end)
        return now_min >= start && now_min < end;
    return now_min >= start || now_min < end;
}

static int parse_args(int argc, char **argv, options_t *o)
{
    static const struct option longopts[] = {
        {"high",            required_argument, 0, 'H'},
        {"low",             required_argument, 0, 'L'},
        {"interval",        required_argument, 0, 'i'},
        {"mode",            required_argument, 0, 'm'},
        {"step",            required_argument, 0, 's'},
        {"floor",           required_argument, 0, 'f'},
        {"night",           no_argument,       0, 'N'},
        {"night-start",     required_argument, 0, 1001},
        {"night-end",       required_argument, 0, 1002},
        {"weekend",         no_argument,       0, 'W'},
        {"warm-margin",     required_argument, 0, 1003},
        {"critical-margin", required_argument, 0, 1004},
        {"cool-confirm",    required_argument, 0, 1005},
        {"rise-rate",       required_argument, 0, 1006},
        {"fall-rate",       required_argument, 0, 1007},
        {"socket",          required_argument, 0, 'S'},
        {"no-control",      no_argument,       0, 'C'},
        {"dry-run",         no_argument,       0, 'n'},
        {"verbose",         no_argument,       0, 'v'},
        {"mock-temp",       required_argument, 0, 1008},
        {"mock-limit",      no_argument,       0, 1009},
        {"help",            no_argument,       0, 'h'},
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
    o->weekend_enable  = 0;
    o->sock_path       = DEFAULT_SOCK_PATH;
    o->no_control      = 0;
    o->warm_margin     = DEFAULT_WARM_MARGIN_C;
    o->critical_margin = DEFAULT_CRITICAL_MARGIN_C;
    o->cool_confirm    = DEFAULT_COOL_CONFIRM;
    o->rise_rate       = DEFAULT_RISE_RATE;
    o->fall_rate       = DEFAULT_FALL_RATE;
    o->mock_temp_path  = NULL;
    o->mock_limit      = 0;

    int floor_arg = -1;
    int c;
    while ((c = getopt_long(argc, argv, "H:L:i:m:s:f:NWS:Cnvh", longopts, NULL)) != -1) {
        switch (c) {
        case 'H': o->high_c = atoi(optarg); break;
        case 'L': o->low_c = atoi(optarg); break;
        case 'i': o->interval_ms = atoi(optarg); break;
        case 'm':
            if (limiter_parse_mode(optarg, &o->mode) != 0) {
                fprintf(stderr, "无效的 --mode: %s\n", optarg);
                return -1;
            }
            break;
        case 's': o->step = atoi(optarg); break;
        case 'f': floor_arg = atoi(optarg); break;
        case 'N': o->night_enable = 1; break;
        case 'W': o->weekend_enable = 1; break;
        case 1001:
            if (parse_hhmm(optarg, &o->night_start_min) != 0) {
                fprintf(stderr, "无效的 --night-start: %s\n", optarg);
                return -1;
            }
            break;
        case 1002:
            if (parse_hhmm(optarg, &o->night_end_min) != 0) {
                fprintf(stderr, "无效的 --night-end: %s\n", optarg);
                return -1;
            }
            break;
        case 1003: o->warm_margin = atoi(optarg); break;
        case 1004: o->critical_margin = atoi(optarg); break;
        case 1005: o->cool_confirm = atoi(optarg); break;
        case 1006: o->rise_rate = atol(optarg); break;
        case 1007: o->fall_rate = atol(optarg); break;
        case 'n': o->dry_run = 1; break;
        case 'v': o->verbose = 1; break;
        case 'S': o->sock_path = optarg; break;
        case 'C': o->no_control = 1; break;
        case 1008: o->mock_temp_path = optarg; break;
        case 1009: o->mock_limit = 1; break;
        case 'h': usage(argv[0]); exit(0);
        default:  return -1;
        }
    }

    if (o->step < 0)
        o->step = (o->mode == LIMIT_MODE_FREQ) ? DEFAULT_STEP_MHZ : DEFAULT_STEP_W;

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
    if (o->warm_margin <= 0) {
        fprintf(stderr, "错误: warm-margin 必须为正 (%d)\n", o->warm_margin);
        return -1;
    }
    if (o->critical_margin <= 0) {
        fprintf(stderr, "错误: critical-margin 必须为正 (%d)\n", o->critical_margin);
        return -1;
    }
    if (o->cool_confirm < 1) {
        fprintf(stderr, "错误: cool-confirm 必须 >=1 (%d)\n", o->cool_confirm);
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

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    thermal_source_t src;
    if (opt.mock_temp_path) {
        if (thermal_select_mock(&src, opt.mock_temp_path) != 0) {
            fprintf(stderr, "mock 温度源初始化失败。\n");
            return 1;
        }
    } else {
        if (thermal_select(&src) != 0) {
            fprintf(stderr, "无法选定温度源，退出。\n");
            return 1;
        }
    }

    long floor_freq = (opt.mode == LIMIT_MODE_FREQ) ? opt.floor : 0;
    long floor_pow  = (opt.mode == LIMIT_MODE_POWER) ? opt.floor : 0;

    limiter_t lm;
    int lrc = limiter_init(&lm, opt.mode, opt.dry_run, floor_freq, floor_pow,
                            opt.mock_limit);
    if (lrc < 0) {
        fprintf(stderr, "limiter 初始化失败，退出。\n");
        return 1;
    }

    guard_config_t cfg = {
        .high_c          = opt.high_c,
        .low_c           = opt.low_c,
        .interval_ms     = opt.interval_ms,
        .step            = opt.step,
        .night_enable    = opt.night_enable,
        .night_start_min = opt.night_start_min,
        .night_end_min   = opt.night_end_min,
        .weekend_enable  = opt.weekend_enable,
        .warm_margin     = opt.warm_margin,
        .critical_margin = opt.critical_margin,
        .cool_confirm    = opt.cool_confirm,
        .rise_rate       = opt.rise_rate,
        .fall_rate       = opt.fall_rate,
    };

    guard_runtime_t rt;
    memset(&rt, 0, sizeof(rt));
    rt.cfg     = &cfg;
    rt.lm      = &lm;
    rt.src     = &src;
    rt.dry_run = opt.dry_run;
    snprintf(rt.last_action, sizeof(rt.last_action), "-");

    scheduler_t sched;
    scheduler_init(&sched);

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
    printf("  阈值   : 高=%d  低=%d\n", cfg.high_c, cfg.low_c);
    printf("  模式   : %s  步长=%d  间隔=%dms\n",
           limiter_mode_name(opt.mode), cfg.step, cfg.interval_ms);
    printf("  调度   : WARM=%d  CRIT=%d  确认=%d轮  上升=%ld 下降=%ld\n",
           cfg.high_c - cfg.warm_margin, cfg.high_c + cfg.critical_margin,
           cfg.cool_confirm, cfg.rise_rate, cfg.fall_rate);
    printf("  控制   : %s\n", desc);
    if (cfg.night_enable)
        printf("  夜间   : %02d:%02d-%02d:%02d\n",
               cfg.night_start_min / 60, cfg.night_start_min % 60,
               cfg.night_end_min   / 60, cfg.night_end_min   % 60);
    if (cfg.weekend_enable)
        printf("  周末   : 周六/周日全天最低\n");
    printf("  接口   : %s\n", ctl_fd >= 0 ? opt.sock_path : "(无)");
    printf("  状态   : %s\n", lm.active ? "可写(将实际限制)"
                                        : (opt.dry_run ? "dry-run(仅打印)"
                                                       : (opt.mock_limit ? "mock(内存模拟)"
                                                                          : "只读监控(无权限)")));
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
        char action[64] = "保持";

        int want_idle = 0;
        struct tm tm;
        time_t now = time(NULL);
        localtime_r(&now, &tm);

        if (cfg.weekend_enable && (tm.tm_wday == 0 || tm.tm_wday == 6))
            want_idle = 1;

        if (!want_idle && cfg.night_enable &&
            cfg.night_start_min != cfg.night_end_min) {
            int now_min = tm.tm_hour * 60 + tm.tm_min;
            want_idle = in_night_window(now_min, cfg.night_start_min,
                                        cfg.night_end_min);
        }

        if (want_idle && !rt.idle_active) {
            limiter_force_floor(&lm);
            limiter_describe(&lm, desc, sizeof(desc));
            rt.idle_active = 1;
            snprintf(action, sizeof(action), "进入强制最低");
        } else if (!want_idle && rt.idle_active) {
            limiter_force_ceiling(&lm);
            limiter_describe(&lm, desc, sizeof(desc));
            rt.idle_active = 0;
            snprintf(action, sizeof(action), "退出强制最低");
        } else if (rt.idle_active) {
            snprintf(action, sizeof(action), "强制最低(保持)");
        } else {
            struct timespec ts_now;
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            scheduler_step(&sched, &cfg, &lm, millic, &ts_now,
                           action, sizeof(action));
            limiter_describe(&lm, desc, sizeof(desc));
        }

        snprintf(rt.last_action, sizeof(rt.last_action), "%s", action);

        if (opt.verbose) {
            temp_zone_t z = scheduler_classify(
                temp_c, cfg.high_c, cfg.low_c + sched.ping_pong_boost,
                cfg.warm_margin, cfg.critical_margin);
            long slope = scheduler_slope_mcs(&sched);
            printf("temp=%ld.%03ld  zone=%s  slope=%ld  动作=%s  %s\n",
                   millic / 1000, millic % 1000,
                   scheduler_zone_name(z), slope, action, desc);
        }

        control_wait(ctl_fd, cfg.interval_ms, &rt);
    }

    printf("\n收到退出信号，恢复原始设置...\n");
    control_stop(ctl_fd, opt.no_control ? NULL : opt.sock_path);
    limiter_free(&lm);
    printf("已恢复，退出。\n");
    return 0;
}
