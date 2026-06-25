#include "config.h"
#include "thermal.h"
#include "limiter.h"

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
        "  -n, --dry-run        不实际写入，仅打印决策\n"
        "  -v, --verbose        打印每次轮询的温度与动作\n"
        "  -h, --help           显示本帮助\n",
        prog, DEFAULT_HIGH_C, DEFAULT_LOW_C, DEFAULT_INTERVAL_MS);
}

static int parse_args(int argc, char **argv, options_t *o)
{
    static const struct option longopts[] = {
        {"high",     required_argument, 0, 'H'},
        {"low",      required_argument, 0, 'L'},
        {"interval", required_argument, 0, 'i'},
        {"mode",     required_argument, 0, 'm'},
        {"step",     required_argument, 0, 's'},
        {"floor",    required_argument, 0, 'f'},
        {"dry-run",  no_argument,       0, 'n'},
        {"verbose",  no_argument,       0, 'v'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    o->high_c      = DEFAULT_HIGH_C;
    o->low_c       = DEFAULT_LOW_C;
    o->interval_ms = DEFAULT_INTERVAL_MS;
    o->mode        = LIMIT_MODE_FREQ;
    o->step        = -1;
    o->floor       = 0;
    o->dry_run     = 0;
    o->verbose     = 0;

    int floor_arg = -1;
    int c;
    while ((c = getopt_long(argc, argv, "H:L:i:m:s:f:nvh", longopts, NULL)) != -1) {
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
        case 'n': o->dry_run = 1; break;
        case 'v': o->verbose = 1; break;
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
    return 0;
}

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR && !g_stop)
        ;
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

    char desc[160];
    limiter_describe(&lm, desc, sizeof(desc));

    printf("CPU Thermal Guard\n");
    printf("  温度源 : %s (%s)\n", src.type, src.temp_path);
    printf("  阈值   : 高=%d℃  低=%d℃\n", opt.high_c, opt.low_c);
    printf("  模式   : %s  步长=%d  间隔=%dms\n",
           limiter_mode_name(opt.mode), opt.step, opt.interval_ms);
    printf("  控制   : %s\n", desc);
    printf("  状态   : %s\n", lm.active ? "可写(将实际限制)"
                                        : (opt.dry_run ? "dry-run(仅打印)"
                                                       : "只读监控(无权限)"));
    printf("  按 Ctrl-C 退出，退出时自动恢复原始设置。\n\n");

    while (!g_stop) {
        long millic;
        if (thermal_read(&src, &millic) != 0) {
            fprintf(stderr, "读取温度失败，跳过本次。\n");
            sleep_ms(opt.interval_ms);
            continue;
        }

        long temp_c = millic / 1000;
        const char *action = "保持";

        if (temp_c >= opt.high_c) {
            if (limiter_tighten(&lm, opt.step)) {
                limiter_describe(&lm, desc, sizeof(desc));
                action = "收紧";
            } else {
                action = "已至地板";
            }
        } else if (temp_c <= opt.low_c) {
            if (limiter_is_limited(&lm)) {
                if (limiter_relax(&lm, opt.step)) {
                    limiter_describe(&lm, desc, sizeof(desc));
                    action = "放宽";
                }
            }
        }

        if (opt.verbose)
            printf("temp=%ld.%03ld℃  动作=%s  %s\n",
                   millic / 1000, millic % 1000, action, desc);

        sleep_ms(opt.interval_ms);
    }

    printf("\n收到退出信号，恢复原始设置...\n");
    limiter_free(&lm);
    printf("已恢复，退出。\n");
    return 0;
}
