#include "thermal.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/* 读取一个 sysfs 文本文件的首行，去掉结尾换行。成功返回 0。 */
static int read_line(const char *path, char *buf, size_t buflen)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(buf, (int)buflen, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

/* 把候选 zone 信息填入 src（不校验存在性，调用者保证）。 */
static void fill_source(thermal_source_t *src, const char *type, int idx)
{
    snprintf(src->type, sizeof(src->type), "%s", type);
    snprintf(src->temp_path, sizeof(src->temp_path),
             "%s/thermal_zone%d/temp", THERMAL_BASE, idx);
    src->valid = 1;
}

int thermal_select(thermal_source_t *src)
{
    DIR *d = opendir(THERMAL_BASE);
    if (!d) {
        fprintf(stderr, "thermal: 无法打开 %s\n", THERMAL_BASE);
        return -1;
    }

    int preferred = -1, fallback = -1, first = -1;
    char type[64], path[512];
    struct dirent *e;

    while ((e = readdir(d)) != NULL) {
        int idx;
        if (sscanf(e->d_name, "thermal_zone%d", &idx) != 1)
            continue;

        snprintf(path, sizeof(path), "%s/%s/type", THERMAL_BASE, e->d_name);
        if (read_line(path, type, sizeof(type)) != 0)
            continue;

        if (first < 0 || idx < first)
            first = idx; /* 记录编号最小的 zone 作为兜底 */

        if (strcmp(type, PREFERRED_THERMAL_TYPE) == 0 && preferred < 0)
            preferred = idx;
        else if (strcmp(type, FALLBACK_THERMAL_TYPE) == 0 && fallback < 0)
            fallback = idx;
    }
    closedir(d);

    if (preferred >= 0) {
        fill_source(src, PREFERRED_THERMAL_TYPE, preferred);
        return 0;
    }
    if (fallback >= 0) {
        fill_source(src, FALLBACK_THERMAL_TYPE, fallback);
        return 0;
    }
    if (first >= 0) {
        /* 兜底：重新读取该 zone 的 type 名字 */
        snprintf(path, sizeof(path), "%s/thermal_zone%d/type", THERMAL_BASE, first);
        if (read_line(path, type, sizeof(type)) != 0)
            snprintf(type, sizeof(type), "thermal_zone%d", first);
        fill_source(src, type, first);
        return 0;
    }

    fprintf(stderr, "thermal: 未找到任何 thermal zone\n");
    return -1;
}

int thermal_read(const thermal_source_t *src, long *millic)
{
    char buf[64];
    if (!src || !src->valid)
        return -1;
    if (read_line(src->temp_path, buf, sizeof(buf)) != 0)
        return -1;

    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (end == buf)
        return -1;

    *millic = v;
    return 0;
}

int thermal_select_mock(thermal_source_t *src, const char *mock_path)
{
    if (!src || !mock_path)
        return -1;
    memset(src, 0, sizeof(*src));
    snprintf(src->type, sizeof(src->type), "mock");
    snprintf(src->temp_path, sizeof(src->temp_path), "%s", mock_path);
    src->valid = 1;
    return 0;
}
