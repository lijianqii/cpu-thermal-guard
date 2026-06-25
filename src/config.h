#ifndef CONFIG_H
#define CONFIG_H

/* 默认配置常量 */

#define DEFAULT_HIGH_C       80      /* 高阈值（℃）：超过则限制 */
#define DEFAULT_LOW_C        72      /* 低阈值（℃）：低于则放宽 */
#define DEFAULT_INTERVAL_MS  1000    /* 轮询间隔（毫秒） */
#define DEFAULT_STEP_MHZ     200     /* 限频步长（MHz） */
#define DEFAULT_STEP_W       2       /* 限功率步长（W） */

/* sysfs 路径 */
#define THERMAL_BASE   "/sys/class/thermal"
#define CPUFREQ_BASE   "/sys/devices/system/cpu"
#define RAPL_BASE      "/sys/class/powercap"

/* 首选温度传感器 type，按优先级回退 */
#define PREFERRED_THERMAL_TYPE   "x86_pkg_temp"
#define FALLBACK_THERMAL_TYPE    "acpitz"

#endif /* CONFIG_H */
