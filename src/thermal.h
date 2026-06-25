#ifndef THERMAL_H
#define THERMAL_H

/*
 * 温度读取模块。
 * 负责在 /sys/class/thermal 下选定一个代表 CPU 温度的 thermal zone，
 * 之后周期性读取其温度。
 */

/* 已选定的温度源信息 */
typedef struct {
    char  type[64];     /* zone 的 type 字符串，如 "x86_pkg_temp" */
    char  temp_path[256]; /* 该 zone 的 temp 文件路径 */
    int   valid;        /* 是否成功选定 */
} thermal_source_t;

/*
 * 探测并选定一个 CPU 温度源。
 * 优先选 type == PREFERRED_THERMAL_TYPE，其次 FALLBACK_THERMAL_TYPE，
 * 都没有则选 thermal_zone0。
 * 成功返回 0 并填充 *src；失败返回 -1。
 */
int thermal_select(thermal_source_t *src);

/*
 * 读取已选温度源的当前温度，单位：毫摄氏度（millidegree Celsius）。
 * 成功返回 0 并写入 *millic；失败返回 -1。
 */
int thermal_read(const thermal_source_t *src, long *millic);

#endif /* THERMAL_H */
