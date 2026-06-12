#ifndef PROXY_CONFIG_H
#define PROXY_CONFIG_H

#include <stdatomic.h>

#define MAX_BACKENDS 8

typedef struct {
    char        host[256];
    int         port;
    atomic_long request_count;
    atomic_int  is_alive;   /* 1=存活，0=不可用 */
} backend_t;

typedef struct {
    backend_t  backends[MAX_BACKENDS];
    int        count;
    atomic_uint rr_index;   /* round-robin 当前索引 */
} proxy_config_t;

/* 轮询选择一个存活的后端；全部不可用时返回 NULL */
backend_t *pick_backend(proxy_config_t *cfg);

#endif /* PROXY_CONFIG_H */
