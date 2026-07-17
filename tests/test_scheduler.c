/*
 * test_scheduler.c - 分级自适应调度器单元测试
 *
 * 用 mock limiter (内存模拟) 验证 scheduler 的五区间决策、
 * 趋势感知、COLD 确认、乒乓检测、自适应步长等逻辑。
 *
 * 编译: make test
 * 运行: ./tests/test_scheduler
 */
#include "scheduler.h"
#include "limiter.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

static guard_config_t make_cfg(void)
{
    guard_config_t c = {
        .high_c          = 80,
        .low_c           = 72,
        .interval_ms     = 100,
        .step            = 50,
        .night_enable    = 0,
        .night_start_min = 0,
        .night_end_min   = 0,
        .weekend_enable  = 0,
        .warm_margin     = 3,
        .critical_margin = 8,
        .cool_confirm    = 3,
        .rise_rate       = 500,
        .fall_rate       = 300,
    };
    return c;
}

static limiter_t make_mock_limiter(void)
{
    limiter_t lm;
    limiter_init(&lm, LIMIT_MODE_FREQ, 1, 0, 0, 1);
    return lm;
}

static struct timespec mktime_s(int sec)
{
    struct timespec ts = { .tv_sec = sec, .tv_nsec = 0 };
    return ts;
}

static void step_and_check(scheduler_t *s, guard_config_t *cfg, limiter_t *lm,
                           long millic, int sec,
                           const char *expect_action_contains)
{
    struct timespec ts = mktime_s(sec);
    char action[64] = {0};
    scheduler_step(s, cfg, lm, millic, &ts, action, sizeof(action));
    if (expect_action_contains) {
        int ok = strstr(action, expect_action_contains) != NULL;
        CHECK(ok, expect_action_contains);
        if (!ok)
            printf("  got action='%s', expected to contain '%s'\n",
                   action, expect_action_contains);
    }
}

/* 测试 1: COOL 区间保持不变 */
static void test_cool_hold(void)
{
    printf("[test_cool_hold] ");
    guard_config_t cfg = make_cfg();
    limiter_t lm = make_mock_limiter();
    scheduler_t sched;
    scheduler_init(&sched);

    step_and_check(&sched, &cfg, &lm, 75000, 1, NULL);  /* 75C: COOL */
    step_and_check(&sched, &cfg, &lm, 76000, 2, NULL);  /* 76C: COOL */
    step_and_check(&sched, &cfg, &lm, 74000, 3, NULL);  /* 74C: COOL */

    CHECK(lm.freq_current == lm.freq_ceiling, "COOL should not change freq");
    printf("OK\n");
}

/* 测试 2: HOT 区间收紧 + 自适应步长 */
static void test_hot_tighten(void)
{
    printf("[test_hot_tighten] ");
    guard_config_t cfg = make_cfg();
    limiter_t lm = make_mock_limiter();
    scheduler_t sched;
    scheduler_init(&sched);

    /* 先给点历史数据 */
    step_and_check(&sched, &cfg, &lm, 70000, 1, NULL);
    step_and_check(&sched, &cfg, &lm, 73000, 2, NULL);

    /* 82C: HOT, dist=2, mult=1+0=1, step=50MHz */
    long before = lm.freq_current;
    step_and_check(&sched, &cfg, &lm, 82000, 3, "\xe6\x94\xb6\xe7\xb4\xa7");
    long drop = before - lm.freq_current;
    /* 82-80=2, 2/3=0, mult=1, step=50MHz -> drop=50000 kHz */
    CHECK(drop == 50000, "HOT dist=2 should drop 50MHz");
    printf("OK (drop=%ld kHz)\n", drop);
}

/* 测试 3: HOT 更高温度自适应更大步长 */
static void test_hot_adaptive_step(void)
{
    printf("[test_hot_adaptive_step] ");
    guard_config_t cfg = make_cfg();
    limiter_t lm = make_mock_limiter();
    scheduler_t sched;
    scheduler_init(&sched);

    step_and_check(&sched, &cfg, &lm, 70000, 1, NULL);
    step_and_check(&sched, &cfg, &lm, 75000, 2, NULL);

    /* 86C: HOT, dist=6, mult=1+2=3, step=50*3=150MHz */
    long before = lm.freq_current;
    step_and_check(&sched, &cfg, &lm, 86000, 3, NULL);
    long drop = before - lm.freq_current;
    CHECK(drop == 150000, "HOT dist=6 should drop 150MHz");
    printf("OK (drop=%ld kHz)\n", drop);
}

/* 测试 4: CRITICAL 区间直接压到地板 */
static void test_critical_floor(void)
{
    printf("[test_critical_floor] ");
    guard_config_t cfg = make_cfg();
    limiter_t lm = make_mock_limiter();
    scheduler_t sched;
    scheduler_init(&sched);

    step_and_check(&sched, &cfg, &lm, 70000, 1, NULL);

    /* 89C: >= 80+8=88, CRITICAL */
    step_and_check(&sched, &cfg, &lm, 89000, 2, "\xe7\xb4\xa7\xe6\x80\xa5");
    CHECK(lm.freq_current == lm.freq_floor, "CRITICAL should force floor");
    printf("OK (freq=%ld, floor=%ld)\n", lm.freq_current, lm.freq_floor);
}

/* 测试 5: COLD 确认 - 需要连续 N 轮才放宽 */
static void test_cold_confirm(void)
{
    printf("[test_cold_confirm] ");
    guard_config_t cfg = make_cfg();
    limiter_t lm = make_mock_limiter();
    scheduler_t sched;
    scheduler_init(&sched);

    /* 先收紧到非满频 */
    step_and_check(&sched, &cfg, &lm, 70000, 1, NULL);
    step_and_check(&sched, &cfg, &lm, 75000, 2, NULL);
    step_and_check(&sched, &cfg, &lm, 85000, 3, NULL);  /* HOT: tighten */
    CHECK(lm.freq_current < lm.freq_ceiling, "should be limited after HOT");

    /* COLD 1: 不应放宽 (cool_confirm=3) */
    long before = lm.freq_current;
    step_and_check(&sched, &cfg, &lm, 60000, 4, NULL);
    CHECK(lm.freq_current == before, "COLD round 1 should not relax");

    /* COLD 2: 不应放宽 */
    step_and_check(&sched, &cfg, &lm, 60000, 5, NULL);
    CHECK(lm.freq_current == before, "COLD round 2 should not relax");

    /* COLD 3: 应该放宽 */
    step_and_check(&sched, &cfg, &lm, 60000, 6, "\xe6\x94\xbe\xe5\xae\xbd");
    CHECK(lm.freq_current > before, "COLD round 3 should relax");
    printf("OK\n");
}

/* 测试 6: 乒乓检测 - 收紧放宽交替后 low 被抬高 */
static void test_ping_pong(void)
{
    printf("[test_ping_pong] ");
    guard_config_t cfg = make_cfg();
    limiter_t lm = make_mock_limiter();
    scheduler_t sched;
    scheduler_init(&sched);

    /* 制造乒乓: HOT 收紧 -> COLD 放宽 -> HOT 收紧 -> COLD 放宽 */
    step_and_check(&sched, &cfg, &lm, 70000, 1, NULL);
    step_and_check(&sched, &cfg, &lm, 75000, 2, NULL);

    /* 收紧 */
    step_and_check(&sched, &cfg, &lm, 82000, 3, NULL);
    /* COLD x3 放宽 */
    step_and_check(&sched, &cfg, &lm, 60000, 4, NULL);
    step_and_check(&sched, &cfg, &lm, 60000, 5, NULL);
    step_and_check(&sched, &cfg, &lm, 60000, 6, NULL);  /* relax */

    /* 收紧 */
    step_and_check(&sched, &cfg, &lm, 82000, 7, NULL);
    /* COLD x3 放宽 */
    step_and_check(&sched, &cfg, &lm, 60000, 8, NULL);
    step_and_check(&sched, &cfg, &lm, 60000, 9, NULL);
    step_and_check(&sched, &cfg, &lm, 60000, 10, NULL);  /* relax */

    /* 应检测到乒乓，ping_pong_boost 应 > 0 */
    CHECK(sched.ping_pong_boost > 0, "ping_pong_boost should be set");
    printf("OK (boost=%d)\n", sched.ping_pong_boost);
}

/* 测试 7: WARM 区间趋势上升时预警收紧 */
static void test_warm_trend(void)
{
    printf("[test_warm_trend] ");
    guard_config_t cfg = make_cfg();
    limiter_t lm = make_mock_limiter();
    scheduler_t sched;
    scheduler_init(&sched);

    /* 从低温度快速上升到 78C (WARM: 77~80) */
    /* sec=1: 50C, sec=2: 64C, sec=3: 78C -> slope = (78000-50000)/2s = 14000 mC/s >> 500 */
    step_and_check(&sched, &cfg, &lm, 50000, 1, NULL);  /* 50C: COLD */
    step_and_check(&sched, &cfg, &lm, 64000, 2, NULL);  /* 64C: still COLD */
    step_and_check(&sched, &cfg, &lm, 78000, 3, NULL);  /* 78C: WARM + fast rise */

    /* slope ~ (78000-50000)/0.2 = 140000 mC/s >> 500, should preempt tighten */
    CHECK(lm.freq_current < lm.freq_ceiling, "WARM with fast rise should tighten");
    printf("OK\n");
}

/* 测试 8: WARM 区间无趋势时不收紧 */
static void test_warm_stable(void)
{
    printf("[test_warm_stable] ");
    guard_config_t cfg = make_cfg();
    limiter_t lm = make_mock_limiter();
    scheduler_t sched;
    scheduler_init(&sched);

    /* 稳定在 78C (WARM) - 无明显上升趋势 */
    step_and_check(&sched, &cfg, &lm, 78000, 1, NULL);
    step_and_check(&sched, &cfg, &lm, 78000, 2, NULL);
    step_and_check(&sched, &cfg, &lm, 78000, 3, NULL);
    step_and_check(&sched, &cfg, &lm, 78000, 4, NULL);

    CHECK(lm.freq_current == lm.freq_ceiling, "WARM stable should not tighten");
    printf("OK\n");
}

/* 测试 9: 下降趋势加速放宽 */
static void test_fall_rate_accel(void)
{
    printf("[test_fall_rate_accel] ");
    guard_config_t cfg = make_cfg();
    limiter_t lm = make_mock_limiter();
    scheduler_t sched;
    scheduler_init(&sched);

    /* 先收紧 */
    step_and_check(&sched, &cfg, &lm, 70000, 1, NULL);
    step_and_check(&sched, &cfg, &lm, 75000, 2, NULL);
    step_and_check(&sched, &cfg, &lm, 85000, 3, NULL);  /* HOT tighten */

    /* 快速下降到 COLD */
    step_and_check(&sched, &cfg, &lm, 70000, 4, NULL);
    step_and_check(&sched, &cfg, &lm, 60000, 5, NULL);

    /* COLD 确认第 3 次，且 slope 很负 -> step*2 */
    long before = lm.freq_current;
    step_and_check(&sched, &cfg, &lm, 50000, 6, "\xe6\x94\xbe\xe5\xae\xbd");
    long gain = lm.freq_current - before;
    /* step=50, 但 fall_rate 触发 -> 100MHz = 100000 kHz */
    CHECK(gain >= 50000, "fall_rate should relax more than 1 step");
    printf("OK (gain=%ld kHz)\n", gain);
}

/* 测试 10: zone 分类正确性 */
static void test_zone_classify(void)
{
    printf("[test_zone_classify] ");
    CHECK(scheduler_classify(60, 80, 72, 3, 8) == ZONE_COLD, "60C -> COLD");
    CHECK(scheduler_classify(72, 80, 72, 3, 8) == ZONE_COLD, "72C -> COLD (boundary)");
    CHECK(scheduler_classify(73, 80, 72, 3, 8) == ZONE_COOL, "73C -> COOL");
    CHECK(scheduler_classify(75, 80, 72, 3, 8) == ZONE_COOL, "75C -> COOL");
    CHECK(scheduler_classify(77, 80, 72, 3, 8) == ZONE_WARM, "77C -> WARM");
    CHECK(scheduler_classify(79, 80, 72, 3, 8) == ZONE_WARM, "79C -> WARM");
    CHECK(scheduler_classify(80, 80, 72, 3, 8) == ZONE_HOT, "80C -> HOT");
    CHECK(scheduler_classify(85, 80, 72, 3, 8) == ZONE_HOT, "85C -> HOT");
    CHECK(scheduler_classify(88, 80, 72, 3, 8) == ZONE_CRITICAL, "88C -> CRITICAL");
    CHECK(scheduler_classify(95, 80, 72, 3, 8) == ZONE_CRITICAL, "95C -> CRITICAL");
    printf("OK\n");
}

/* 测试 11: power 模式 mock limiter */
static void test_power_mode(void)
{
    printf("[test_power_mode] ");
    guard_config_t cfg = make_cfg();
    limiter_t lm;
    limiter_init(&lm, LIMIT_MODE_POWER, 1, 0, 0, 1);
    scheduler_t sched;
    scheduler_init(&sched);

    step_and_check(&sched, &cfg, &lm, 70000, 1, NULL);
    step_and_check(&sched, &cfg, &lm, 75000, 2, NULL);

    long before = lm.power_current;
    step_and_check(&sched, &cfg, &lm, 85000, 3, "\xe6\x94\xb6\xe7\xb4\xa7");
    CHECK(lm.power_current < before, "power mode should tighten");
    printf("OK (power %ld -> %ld uW)\n", before, lm.power_current);
}

int main(void)
{
    printf("=== Scheduler Unit Tests ===\n\n");

    test_zone_classify();
    test_cool_hold();
    test_hot_tighten();
    test_hot_adaptive_step();
    test_critical_floor();
    test_cold_confirm();
    test_warm_trend();
    test_warm_stable();
    test_fall_rate_accel();
    test_ping_pong();
    test_power_mode();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
