#include "limiter.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>

static long clampl(long v, long lo, long hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int read_long(const char *path, long *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long v;
    int n = fscanf(f, "%ld", &v);
    fclose(f);
    if (n != 1) return -1;
    *out = v;
    return 0;
}

/* 写入 sysfs，成功返回 0，失败返回 -errno。 */
static int write_long(const char *path, long v)
{
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%ld", v);
    if (len <= 0 || len >= (int)sizeof(buf)) return -EINVAL;
    FILE *f = fopen(path, "w");
    if (!f) return -errno;
    size_t w = fwrite(buf, 1, (size_t)len, f);
    int rc = 0;
    if (w != (size_t)len) rc = ferror(f) ? -errno : -EIO;
    if (fclose(f) != 0 && rc == 0) rc = -errno;
    return rc;
}

int limiter_parse_mode(const char *s, limit_mode_t *out)
{
    if (!s || !out) return -1;
    if (strcmp(s, "freq") == 0)  { *out = LIMIT_MODE_FREQ;  return 0; }
    if (strcmp(s, "power") == 0) { *out = LIMIT_MODE_POWER; return 0; }
    return -1;
}

const char *limiter_mode_name(limit_mode_t m)
{
    switch (m) {
    case LIMIT_MODE_FREQ:  return "freq";
    case LIMIT_MODE_POWER: return "power";
    default:               return "unknown";
    }
}

static int init_freq(limiter_t *lm, long freq_floor_khz)
{
    DIR *d = opendir(CPUFREQ_BASE);
    if (!d) {
        fprintf(stderr, "limiter: 无法打开 %s\n", CPUFREQ_BASE);
        return -1;
    }
    int idxs[1024];
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < 1024) {
        int idx;
        char tail;
        if (sscanf(e->d_name, "cpu%d%c", &idx, &tail) != 1)
            continue;
        idxs[n++] = idx;
    }
    closedir(d);
    if (n == 0) {
        fprintf(stderr, "limiter: 未发现任何 CPU 核心\n");
        return -1;
    }
    lm->freq_nodes = calloc(n, sizeof(freq_node_t));
    if (!lm->freq_nodes) {
        fprintf(stderr, "limiter: 内存分配失败\n");
        return -1;
    }
    long ceiling = 0, hw_min = 0;
    size_t got = 0;
    for (size_t i = 0; i < n; i++) {
        char base[200], maxp[280], infop[280];
        snprintf(base, sizeof(base), "%s/cpu%d/cpufreq", CPUFREQ_BASE, idxs[i]);
        snprintf(maxp, sizeof(maxp), "%s/scaling_max_freq", base);
        long orig;
        if (read_long(maxp, &orig) != 0)
            continue;
        if (ceiling == 0) {
            snprintf(infop, sizeof(infop), "%s/cpuinfo_max_freq", base);
            read_long(infop, &ceiling);
            snprintf(infop, sizeof(infop), "%s/cpuinfo_min_freq", base);
            read_long(infop, &hw_min);
        }
        snprintf(lm->freq_nodes[got].path, sizeof(lm->freq_nodes[got].path), "%s", maxp);
        lm->freq_nodes[got].orig = orig;
        got++;
    }
    if (got == 0) {
        fprintf(stderr, "limiter: 未发现可用的 cpufreq 节点\n");
        free(lm->freq_nodes);
        lm->freq_nodes = NULL;
        return -1;
    }
    lm->n_freq = got;
    if (ceiling <= 0)
        ceiling = lm->freq_nodes[0].orig;
    lm->freq_ceiling = ceiling;
    if (hw_min <= 0)
        hw_min = ceiling / 4;
    long floor = (freq_floor_khz > 0) ? freq_floor_khz : hw_min;
    lm->freq_floor   = clampl(floor, hw_min, ceiling);
    lm->freq_current = ceiling;
    return 0;
}

static int init_power(limiter_t *lm, long power_floor_uw)
{
    char chosen[320] = {0};
    DIR *d = opendir(RAPL_BASE);
    if (!d) {
        fprintf(stderr, "limiter: 无法打开 %s\n", RAPL_BASE);
        return -1;
    }
    struct dirent *e;
    int best = 1 << 30;
    while ((e = readdir(d)) != NULL) {
        int idx;
        if (sscanf(e->d_name, "intel-rapl:%d", &idx) != 1)
            continue;
        if (strchr(e->d_name, ':') != strrchr(e->d_name, ':'))
            continue;
        char namep[360], name[64];
        snprintf(namep, sizeof(namep), "%s/%s/name", RAPL_BASE, e->d_name);
        FILE *f = fopen(namep, "r");
        if (!f)
            continue;
        if (fgets(name, sizeof(name), f))
            name[strcspn(name, "\n")] = '\0';
        else
            name[0] = '\0';
        fclose(f);
        if (strncmp(name, "package", 7) == 0 && idx < best) {
            best = idx;
            snprintf(chosen, sizeof(chosen), "%s/%s", RAPL_BASE, e->d_name);
        }
    }
    closedir(d);
    if (chosen[0] == '\0') {
        fprintf(stderr, "limiter: 未发现 RAPL package 功率域\n");
        return -1;
    }
    snprintf(lm->power_path, sizeof(lm->power_path),
             "%s/constraint_0_power_limit_uw", chosen);
    long orig;
    if (read_long(lm->power_path, &orig) != 0 || orig <= 0) {
        fprintf(stderr, "limiter: 无法读取功率上限 %s\n", lm->power_path);
        return -1;
    }
    lm->power_orig    = orig;
    lm->power_ceiling = orig;
    lm->power_current = orig;
    long floor = (power_floor_uw > 0) ? power_floor_uw : (orig * 2 / 5);
    lm->power_floor = clampl(floor, 1000000L, orig);
    return 0;
}

static int probe_writable(const limiter_t *lm)
{
    if (lm->mode == LIMIT_MODE_FREQ) {
        if (lm->n_freq == 0)
            return 0;
        return access(lm->freq_nodes[0].path, W_OK) == 0;
    }
    return access(lm->power_path, W_OK) == 0;
}

int limiter_init(limiter_t *lm, limit_mode_t mode, int dry_run,
                 long freq_floor_khz, long power_floor_uw)
{
    memset(lm, 0, sizeof(*lm));
    lm->mode    = mode;
    lm->dry_run = dry_run;
    lm->active  = 0;

    int rc = (mode == LIMIT_MODE_FREQ)
                 ? init_freq(lm, freq_floor_khz)
                 : init_power(lm, power_floor_uw);
    if (rc != 0)
        return -1;

    if (dry_run) {
        lm->active = 0;
        return 0;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "limiter: 非 root，降级为只读监控(不修改 sysfs)。\n");
        lm->active = 0;
        return 0;
    }
    if (!probe_writable(lm)) {
        fprintf(stderr, "limiter: 控制节点不可写，降级为只读监控。\n");
        lm->active = 0;
        return 0;
    }
    lm->active = 1;
    return 1;
}

static int apply_freq(limiter_t *lm, long target_khz)
{
    target_khz = clampl(target_khz, lm->freq_floor, lm->freq_ceiling);
    if (target_khz == lm->freq_current)
        return 0;
    if (!lm->active) {
        lm->freq_current = target_khz;
        return 1;
    }
    int any_fail = 0;
    for (size_t i = 0; i < lm->n_freq; i++) {
        int rc = write_long(lm->freq_nodes[i].path, target_khz);
        if (rc != 0) {
            fprintf(stderr, "limiter: 写 %s 失败: %s\n",
                    lm->freq_nodes[i].path, strerror(-rc));
            any_fail = 1;
        }
    }
    lm->freq_current = target_khz;
    return !any_fail;
}

static int apply_power(limiter_t *lm, long target_uw)
{
    target_uw = clampl(target_uw, lm->power_floor, lm->power_ceiling);
    if (target_uw == lm->power_current)
        return 0;
    if (!lm->active) {
        lm->power_current = target_uw;
        return 1;
    }
    int rc = write_long(lm->power_path, target_uw);
    if (rc != 0) {
        fprintf(stderr, "limiter: 写 %s 失败: %s\n",
                lm->power_path, strerror(-rc));
        return 0;
    }
    lm->power_current = target_uw;
    return 1;
}

int limiter_tighten(limiter_t *lm, int step)
{
    if (lm->mode == LIMIT_MODE_FREQ)
        return apply_freq(lm, lm->freq_current - (long)step * 1000L);
    return apply_power(lm, lm->power_current - (long)step * 1000000L);
}

int limiter_relax(limiter_t *lm, int step)
{
    if (lm->mode == LIMIT_MODE_FREQ)
        return apply_freq(lm, lm->freq_current + (long)step * 1000L);
    return apply_power(lm, lm->power_current + (long)step * 1000000L);
}

int limiter_force_floor(limiter_t *lm)
{
    if (lm->mode == LIMIT_MODE_FREQ)
        return apply_freq(lm, lm->freq_floor);
    return apply_power(lm, lm->power_floor);
}

int limiter_force_ceiling(limiter_t *lm)
{
    if (lm->mode == LIMIT_MODE_FREQ)
        return apply_freq(lm, lm->freq_ceiling);
    return apply_power(lm, lm->power_ceiling);
}

int limiter_is_limited(const limiter_t *lm)
{
    if (lm->mode == LIMIT_MODE_FREQ)
        return lm->freq_current < lm->freq_ceiling;
    return lm->power_current < lm->power_ceiling;
}

void limiter_describe(const limiter_t *lm, char *buf, size_t buflen)
{
    if (lm->mode == LIMIT_MODE_FREQ)
        snprintf(buf, buflen, "max_freq=%ld MHz (上限 %ld, 地板 %ld)",
                 lm->freq_current / 1000, lm->freq_ceiling / 1000,
                 lm->freq_floor / 1000);
    else
        snprintf(buf, buflen, "power_limit=%.1f W (原始 %.1f, 地板 %.1f)",
                 lm->power_current / 1e6, lm->power_orig / 1e6,
                 lm->power_floor / 1e6);
}

void limiter_restore(limiter_t *lm)
{
    if (lm->mode == LIMIT_MODE_FREQ) {
        if (!lm->freq_nodes)
            return;
        if (lm->active) {
            for (size_t i = 0; i < lm->n_freq; i++)
                write_long(lm->freq_nodes[i].path, lm->freq_nodes[i].orig);
        }
        lm->freq_current = lm->freq_ceiling;
    } else {
        if (lm->power_path[0] == '\0')
            return;
        if (lm->active)
            write_long(lm->power_path, lm->power_orig);
        lm->power_current = lm->power_orig;
    }
}

void limiter_free(limiter_t *lm)
{
    limiter_restore(lm);
    free(lm->freq_nodes);
    lm->freq_nodes = NULL;
    lm->n_freq = 0;
}

