#include "scheduler.h"
#include "config.h"

#include <string.h>
#include <stdio.h>

void scheduler_init(scheduler_t *s)
{
    memset(s, 0, sizeof(*s));
}

void scheduler_push(scheduler_t *s, long millic, const struct timespec *now)
{
    s->hist.temps[s->hist.head] = millic;
    s->hist.times[s->hist.head] = *now;
    s->hist.head = (s->hist.head + 1) % HIST_SIZE;
    if (s->hist.count < HIST_SIZE)
        s->hist.count++;
}

long scheduler_slope_mcs(const scheduler_t *s)
{
    if (s->hist.count < 2)
        return 0;
    int newest = (s->hist.head - 1 + HIST_SIZE) % HIST_SIZE;
    int oldest = (s->hist.head - s->hist.count + HIST_SIZE) % HIST_SIZE;
    long dtemp = s->hist.temps[newest] - s->hist.temps[oldest];
    long dsec  = s->hist.times[newest].tv_sec - s->hist.times[oldest].tv_sec;
    long dnsec = s->hist.times[newest].tv_nsec - s->hist.times[oldest].tv_nsec;
    long dmsec = dsec * 1000L + dnsec / 1000000L;
    if (dmsec <= 0)
        return 0;
    return dtemp * 1000L / dmsec;
}

temp_zone_t scheduler_classify(long temp_c, int high, int low,
                               int warm_margin, int critical_margin)
{
    int warm_low = high - warm_margin;
    int crit     = high + critical_margin;

    if (temp_c >= crit)
        return ZONE_CRITICAL;
    if (temp_c >= high)
        return ZONE_HOT;
    if (temp_c >= warm_low)
        return ZONE_WARM;
    if (temp_c > low)
        return ZONE_COOL;
    return ZONE_COLD;
}

const char *scheduler_zone_name(temp_zone_t z)
{
    switch (z) {
    case ZONE_COLD:     return "COLD";
    case ZONE_COOL:     return "COOL";
    case ZONE_WARM:     return "WARM";
    case ZONE_HOT:      return "HOT";
    case ZONE_CRITICAL: return "CRITICAL";
    }
    return "?";
}

static void log_action(scheduler_t *s, const char *action)
{
    snprintf(s->action_log[s->action_log_head], 32, "%s", action);
    s->action_log_head = (s->action_log_head + 1) % ACTION_LOG_SIZE;
    if (s->action_log_count < ACTION_LOG_SIZE)
        s->action_log_count++;
}

static int detect_ping_pong(scheduler_t *s)
{
    if (s->action_log_count < 4)
        return 0;
    int tighten = 0, relax = 0;
    int start = (s->action_log_head - s->action_log_count + ACTION_LOG_SIZE) % ACTION_LOG_SIZE;
    for (int i = 0; i < s->action_log_count; i++) {
        int idx = (start + i) % ACTION_LOG_SIZE;
        if (strstr(s->action_log[idx], "收紧"))
            tighten++;
        else if (strstr(s->action_log[idx], "放宽"))
            relax++;
    }
    return (tighten >= 2 && relax >= 2);
}

static int hot_step_multiplier(int distance_c)
{
    int raw = 1 + distance_c / 3;
    if (raw > 4) raw = 4;
    if (raw < 1) raw = 1;
    return raw;
}

int scheduler_step(scheduler_t *s, const guard_config_t *cfg, limiter_t *lm,
                   long millic, const struct timespec *now,
                   char *action_out, size_t action_len)
{
    scheduler_push(s, millic, now);

    long temp_c = millic / 1000;
    int  eff_low = cfg->low_c + s->ping_pong_boost;

    temp_zone_t zone = scheduler_classify(temp_c, cfg->high_c, eff_low,
                                           cfg->warm_margin, cfg->critical_margin);
    long slope = scheduler_slope_mcs(s);

    int  changed = 0;
    const char *action = "保持";

    switch (zone) {
    case ZONE_COLD:
        s->cool_count++;
        if (s->cool_count >= cfg->cool_confirm) {
            if (limiter_is_limited(lm)) {
                int step = cfg->step;
                if (slope < -cfg->fall_rate)
                    step *= 2;
                if (limiter_relax(lm, step)) {
                    changed = 1;
                    action = "放宽";
                }
            }
        }
        break;

    case ZONE_COOL:
        s->cool_count = 0;
        break;

    case ZONE_WARM:
        s->cool_count = 0;
        if (slope > cfg->rise_rate) {
            if (limiter_tighten(lm, cfg->step)) {
                changed = 1;
                action = "预警收紧";
            }
        }
        break;

    case ZONE_HOT: {
        s->cool_count = 0;
        int dist = (int)(temp_c - cfg->high_c);
        int mult = hot_step_multiplier(dist);
        int eff_step = cfg->step * mult;
        if (limiter_tighten(lm, eff_step)) {
            changed = 1;
            action = "收紧";
        } else {
            action = "已至地板";
        }
        break;
    }

    case ZONE_CRITICAL:
        s->cool_count = 0;
        limiter_force_floor(lm);
        changed = 1;
        action = "紧急降频";
        break;
    }

    log_action(s, action);

    if (detect_ping_pong(s) && s->ping_pong_boost == 0) {
        s->ping_pong_boost = PINGPONG_BOOST_C;
        s->cool_count = 0;  /* 重置确认计数，需在抬高后的阈值下重新稳定 */
    }

    if (s->ping_pong_boost > 0 && s->cool_count >= cfg->cool_confirm)
        s->ping_pong_boost = 0;

    if (action_out)
        snprintf(action_out, action_len, "%s", action);

    return changed;
}
