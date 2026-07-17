#ifndef CONFIG_H
#define CONFIG_H

/* 默认配置常量 */

#define DEFAULT_HIGH_C       80      /* 高阈值（℃）：超过则限制 */
#define DEFAULT_LOW_C        72      /* 低阈值（℃）：低于则放宽 */
#define DEFAULT_INTERVAL_MS  1000    /* 轮询间隔（毫秒） */
#define DEFAULT_STEP_MHZ     50      /* 限频步长（MHz） */
#define DEFAULT_STEP_W       2       /* 限功率步长（W） */

/* 夜间空闲时段默认窗口（跨午夜，单位分钟） */
#define DEFAULT_NIGHT_START_MIN   (23*60 + 0)   /* 23:00 */
#define DEFAULT_NIGHT_END_MIN     (7*60 + 0)    /* 07:00 */

/* ---- 分级自适应调度参数 ---- */
#define DEFAULT_WARM_MARGIN_C      3     /* WARM 区下沿偏移：high - margin */
#define DEFAULT_CRITICAL_MARGIN_C  8     /* CRITICAL 区上沿偏移：high + margin */
#define DEFAULT_COOL_CONFIRM       3     /* COLD 连续确认轮数 */
#define DEFAULT_RISE_RATE         500   /* 上升趋势触发阈值 (milli°C/s, 500=0.5°C/s) */
#define DEFAULT_FALL_RATE         300   /* 下降趋势加速阈值 (milli°C/s, 300=0.3°C/s) */
#define PINGPONG_BOOST_C           2     /* 乒乓检测时临时抬高 low 的度数 */

/* ---- Mock 测试用默认值 ---- */
#define MOCK_FREQ_CEILING_KHZ   2800000L   /* 2800 MHz */
#define MOCK_FREQ_FLOOR_KHZ      400000L   /*  400 MHz */
#define MOCK_POWER_CEILING_UW  28000000L   /*  28 W */
#define MOCK_POWER_FLOOR_UW   10000000L   /*  10 W */

/* sysfs 路径 */
#define THERMAL_BASE   "/sys/class/thermal"
#define CPUFREQ_BASE   "/sys/devices/system/cpu"
#define RAPL_BASE      "/sys/class/powercap"

/* 首选温度传感器 type，按优先级回退 */
#define PREFERRED_THERMAL_TYPE   "x86_pkg_temp"
#define FALLBACK_THERMAL_TYPE    "acpitz"

#endif /* CONFIG_H */
