#ifndef PROTOCOL_H
#define PROTOCOL_H

/*
 * 守护进程与 ctl 客户端之间的控制协议。
 *
 * 传输: Unix domain socket (SOCK_STREAM)。
 * 报文: 文本行。客户端发送一行请求，服务端返回文本响应后关闭连接。
 *
 * 请求:
 *   GET                 -> 返回全部当前配置/状态 (key=value 多行)
 *   SET <key> <value>   -> 修改一项运行时配置，返回 OK 或 ERR <原因>
 *
 * 可 SET 的 key:
 *   high         高阈值 ℃
 *   low          低阈值 ℃
 *   interval     轮询间隔 ms
 *   step         步长 (freq:MHz / power:W)
 *   night        夜间开关 (on/off/1/0)
 *   night-start  夜间起始 HH:MM
 *   night-end    夜间结束 HH:MM
 *   weekend      周末开关 (on/off/1/0)，周六/周日全天强制最低
 */

#define DEFAULT_SOCK_PATH   "/run/cpu-thermal-guard.sock"

#endif /* PROTOCOL_H */
