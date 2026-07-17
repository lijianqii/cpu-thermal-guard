#ifndef LIMITER_H
#define LIMITER_H

/*
 * 限制模块：超温时收紧、降温后放宽 CPU 性能上限。
 * 支持两种方式：
 *   - LIMIT_MODE_FREQ : 调整每核 cpufreq/scaling_max_freq (kHz)
 *   - LIMIT_MODE_POWER: 调整 RAPL constraint_0_power_limit_uw (uW)
 *
 * 安全设计:
 *   - 启动时保存所有受控节点的原始值。
 *   - 每次写入都 clamp 到 [floor, ceiling] 硬件/配置边界。
 *   - 任何退出路径都调用 limiter_restore() 写回原值。
 *   - 非 root 或写入被拒时降级为只读监控 (active=0)，绝不盲写。
 */

#include <stddef.h>

typedef enum {
    LIMIT_MODE_FREQ = 0,
    LIMIT_MODE_POWER
} limit_mode_t;

/* 每核频率控制项 */
typedef struct {
    char path[320];   /* scaling_max_freq 路径 */
    long orig;        /* 原始值 (kHz) */
} freq_node_t;

typedef struct {
    limit_mode_t mode;
    int dry_run;      /* 1: 只打印不写入 */
    int active;       /* 1: 实际写入有效 (root 且节点可用) */

    /* ---- 频率模式 ---- */
    freq_node_t *freq_nodes;
    size_t       n_freq;
    long         freq_ceiling;  /* cpuinfo_max_freq (kHz) */
    long         freq_floor;    /* 允许的最低 scaling_max_freq (kHz) */
    long         freq_current;  /* 当前已施加的上限 (kHz) */

    /* ---- 功率模式 ---- */
    char  power_path[256];      /* constraint_0_power_limit_uw 路径 */
    long  power_orig;           /* 原始功率上限 (uW) */
    long  power_ceiling;        /* 不超过原值 (uW) */
    long  power_floor;          /* 允许的最低功率上限 (uW) */
    long  power_current;        /* 当前已施加的功率上限 (uW) */
} limiter_t;

/* 解析模式字符串 "freq"/"power"。成功返回 0。 */
int limiter_parse_mode(const char *s, limit_mode_t *out);
const char *limiter_mode_name(limit_mode_t m);

/*
 * 初始化限制器：探测节点、保存原值、计算边界。
 *   freq_floor_khz: <=0 表示用 cpuinfo_min_freq；否则作为频率地板(clamp 到硬件范围)。
 *   power_floor_uw: <=0 表示默认(原值的 40%)；否则作为功率地板。
 *   mock: 1 = 不访问 sysfs，使用内存模拟值 (用于测试)。
 * 返回:
 *    1  = 就绪且可写 (active)
 *    0  = 降级为只读监控 (dry_run 或非 root 或节点不可写)
 *   -1  = 致命错误 (无法探测到任何受控节点)
 */
int limiter_init(limiter_t *lm, limit_mode_t mode, int dry_run,
                 long freq_floor_khz, long power_floor_uw, int mock);

/* 收紧一档 (step: freq 为 MHz, power 为 W)。返回 1 表示发生了变化。 */
int limiter_tighten(limiter_t *lm, int step);
/* 放宽一档。返回 1 表示发生了变化。 */
int limiter_relax(limiter_t *lm, int step);

/* 强制压到地板（最低上限）。返回 1 表示发生了变化。幂等。 */
int limiter_force_floor(limiter_t *lm);
/* 强制放开到天花板（最高上限）。返回 1 表示发生了变化。幂等。 */
int limiter_force_ceiling(limiter_t *lm);

/* 当前是否已处于受限状态(低于原始/天花板)。 */
int limiter_is_limited(const limiter_t *lm);

/* 把当前上限格式化进 buf，便于打印。 */
void limiter_describe(const limiter_t *lm, char *buf, size_t buflen);

/* 恢复所有节点到原始值。幂等，可在任何退出路径多次调用。 */
void limiter_restore(limiter_t *lm);

/* 释放动态分配的资源(会先 restore)。 */
void limiter_free(limiter_t *lm);

#endif /* LIMITER_H */
