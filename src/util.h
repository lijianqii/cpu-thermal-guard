#ifndef UTIL_H
#define UTIL_H

/*
 * 通用工具函数。
 */

/* 解析 HH:MM 格式字符串为当天分钟数 (0..1439)。
 * 成功返回 0 并写入 *out_min，失败返回 -1。 */
int parse_hhmm(const char *s, int *out_min);

#endif /* UTIL_H */
