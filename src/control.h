#ifndef CONTROL_H
#define CONTROL_H

/*
 * 控制接口: 在守护进程内监听一个 Unix domain socket，
 * 接受 ctl 客户端的 GET/SET 请求来读取/修改运行时配置。
 *
 * 设计为单线程: 主循环用 control_wait() 替代 sleep，
 * 在轮询间隔内用 poll() 处理到来的控制连接，避免加锁。
 */

#include "limiter.h"
#include "thermal.h"

/* 运行时可修改的配置项 (被 ctl SET 修改) */
typedef struct {
    int high_c;
    int low_c;
    int interval_ms;
    int step;
    int night_enable;
    int night_start_min;
    int night_end_min;
    int weekend_enable;       /* 周末全天强制最低 */
} guard_config_t;

/* 守护进程运行时状态: 控制服务与主循环共享 */
typedef struct {
    guard_config_t          *cfg;        /* 可修改配置 */
    limiter_t               *lm;         /* 限制器 (只读其状态用于上报) */
    const thermal_source_t  *src;        /* 温度源 (上报用) */
    int                      dry_run;
    int                      idle_active;   /* 当前是否处于强制最低态(周末或夜间) */
    long                     last_millic;   /* 最近一次读到的温度 (毫度) */
    char                     last_action[64];
} guard_runtime_t;

/*
 * 启动控制 socket 服务。
 *   path: socket 路径。
 * 返回监听 fd (>=0) 成功；<0 失败 (已打印原因，可降级为无控制接口)。
 */
int control_start(const char *path);

/*
 * 在 listen_fd 上等待至多 timeout_ms 毫秒，期间处理所有到来的控制请求。
 * 若 listen_fd < 0 则退化为纯 sleep。
 *   rt: 运行时状态，供 GET 上报与 SET 修改。
 */
void control_wait(int listen_fd, int timeout_ms, guard_runtime_t *rt);

/* 关闭并清理 socket。 */
void control_stop(int listen_fd, const char *path);

#endif /* CONTROL_H */
