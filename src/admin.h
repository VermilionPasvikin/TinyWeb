#ifndef ADMIN_H
#define ADMIN_H

#include <stdatomic.h>
#include <time.h>
#include "csapp.h"

#define RATE_WINDOW 60  /* 滑动窗口大小：60秒 */

typedef struct {
    atomic_int  active_connections;     /* 当前并发连接数 */
    atomic_long total_requests;         /* 总请求计数 */
    atomic_long total_errors;           /* 总错误计数（4xx/5xx） */
    atomic_long bytes_sent;             /* 总发送字节数 */
    time_t      start_time;             /* 服务器启动时间戳 */
    atomic_long req_per_sec[RATE_WINDOW]; /* 每秒请求数环形缓冲 */
    atomic_int  window_idx;             /* 环形缓冲当前索引 */
    time_t      last_window_time;       /* 上次更新窗口的时间戳 */
} server_stats_t;

extern server_stats_t g_stats;

/* 处理 /__admin/PATH 请求，由 doit() 调用 */
void admin_handle(rio_t *rio_p, int fd, const char *method, const char *uri);

/* 更新滑动窗口（每次请求后调用） */
void stats_update_window(void);

#endif /* ADMIN_H */
