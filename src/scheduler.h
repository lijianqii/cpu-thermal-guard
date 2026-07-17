#ifndef SCHEDULER_H
#define SCHEDULER_H

/*
 * 分级自适应调度器。
 *
 * 将简单的"高/低双阈值 + 固定步长"升级为：
 *   1. 五级温度区间 (COLD / COOL / WARM / HOT / CRITICAL)
 *   2. 趋势感知 (温度变化率 slope)
 *   3. HOT 区间自适应步长倍率
 *   4. COLD 稳定性确认 (连续 N 轮才放宽)
 *   5. 乒乓检测与滞回自适应
 */

#include <time.h>
#include <stddef.h>
#include "limiter.h"
#include "control.h"

#define HIST_SIZE        8
#define ACTION_LOG_SIZE  10

typedef enum {
    ZONE_COLD = 0,
    ZONE_COOL,
    ZONE_WARM,
    ZONE_HOT,
    ZONE_CRITICAL
} temp_zone_t;

typedef struct {
    long           temps[HIST_SIZE];   /* milli°C */
    struct timespec times[HIST_SIZE];
    int  head;
    int  count;
} temp_history_t;

typedef struct {
    temp_history_t hist;
    int  cool_count;         /* 连续 COLD 计数 */
    int  ping_pong_boost;    /* 乒乓时临时抬高 low 的度数 (°C) */
    char action_log[ACTION_LOG_SIZE][32];
    int  action_log_head;
    int  action_log_count;
} scheduler_t;

void scheduler_init(scheduler_t *s);

/* 推入一次温度采样 */
void scheduler_push(scheduler_t *s, long millic, const struct timespec *now);

/* 计算温度变化率 (milli°C/s)。数据不足返回 0。 */
long scheduler_slope_mcs(const scheduler_t *s);

/* 温度分级 */
temp_zone_t scheduler_classify(long temp_c, int high, int low,
                               int warm_margin, int critical_margin);

/* 区名称 (中文) */
const char *scheduler_zone_name(temp_zone_t z);

/*
 * 执行一轮调度决策。
 * 返回值写入 action_out (如 "收紧"、"放宽"、"保持" 等)。
 * 返回 1 表示限幅器状态发生变化，0 表示保持。
 */
int scheduler_step(scheduler_t *s, const guard_config_t *cfg, limiter_t *lm,
                   long millic, const struct timespec *now,
                   char *action_out, size_t action_len);

#endif /* SCHEDULER_H */
