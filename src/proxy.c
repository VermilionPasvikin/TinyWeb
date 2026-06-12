#include "csapp.h"
#include "sbuf.h"
#include "proxy_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PROXY_THREAD_COUNT 8
#define PROXY_SBUF_SIZE    32
#define RELAY_TIMEOUT_SEC  30
#define BUF_SIZE           8192

/* ---- 全局状态 ---- */
static sbuf_t          g_sbuf;
static proxy_config_t  g_cfg;

/* 每个工作线程收到的是 {client_fd, client_addr} */
typedef struct {
    int  client_fd;
    char client_ip[INET6_ADDRSTRLEN];
} conn_info_t;

/* ---- 轮询选择存活后端 ---- */
backend_t *pick_backend(proxy_config_t *cfg) {
    int n = cfg->count;
    /* 先原子获取一个基准索引，只递增一次，再线性扫描存活后端 */
    int base = (int)(atomic_fetch_add(&cfg->rr_index, 1) % (unsigned)n);
    for (int i = 0; i < n; i++) {
        int idx = (base + i) % n;
        if (atomic_load(&cfg->backends[idx].is_alive)) {
            return &cfg->backends[idx];
        }
    }
    return NULL; /* 全部不可用 */
}

/* ---- 双向中继（select + 超时） ---- */
static void relay_data(int client_fd, int server_fd) {
    fd_set fds;
    char buf[BUF_SIZE];
    struct timeval tv;
    int maxfd = (client_fd > server_fd ? client_fd : server_fd) + 1;

    while (1) {
        tv.tv_sec  = RELAY_TIMEOUT_SEC;
        tv.tv_usec = 0;
        FD_ZERO(&fds);
        FD_SET(client_fd, &fds);
        FD_SET(server_fd, &fds);

        int ret = select(maxfd, &fds, NULL, NULL, &tv);
        if (ret <= 0)
        {
            printf("chaoshi\n");
            exit(1);
        }; /* 超时或错误 */

        if (FD_ISSET(client_fd, &fds)) {
            ssize_t n = read(client_fd, buf, sizeof(buf));
            if (n <= 0) break;
            Rio_writen(server_fd, buf, n);
        }
        if (FD_ISSET(server_fd, &fds)) {
            ssize_t n = read(server_fd, buf, sizeof(buf));
            if (n <= 0) break;
            Rio_writen(client_fd, buf, n);
        }
    }
}

/* ---- 转发单个请求 ---- */
static void forward_request(int client_fd, const char *client_ip) {
    backend_t *backend = pick_backend(&g_cfg);
    if (!backend) {
        /* 无可用后端，返回 503 */
        const char *resp =
            "HTTP/1.0 503 Service Unavailable\r\n"
            "Content-type: text/plain\r\n"
            "Content-length: 22\r\n\r\n"
            "No backends available\n";
        Rio_writen(client_fd, (void *)resp, strlen(resp));
        return;
    }

    /* 连接后端 */
    int server_fd = open_clientfd(backend->host, backend->port);
    if (server_fd < 0) {
        atomic_store(&backend->is_alive, 0);
        const char *resp =
            "HTTP/1.0 502 Bad Gateway\r\n"
            "Content-type: text/plain\r\n"
            "Content-length: 16\r\n\r\n"
            "Backend error\n";
        Rio_writen(client_fd, (void *)resp, strlen(resp));
        return;
    }

    /* 读取客户端请求头并转发（修改 Host:，追加 X-Forwarded-For:） */
    rio_t rio = {0};
    Rio_readinitb(&rio, client_fd);
    char buf[MAXLINE];
    char first_line[MAXLINE] = {0};
    char uri_for_log[MAXLINE] = "/";

    /* 读第一行（请求行） */
    if (Rio_readlineb(&rio, buf, MAXLINE) <= 0)
    {
        Close(server_fd);
        return;
    }
    strncpy(first_line, buf, MAXLINE - 1);
    /* 提取 URI 用于日志 */
    sscanf(buf, "%*s %s", uri_for_log);
    Rio_writen(server_fd, buf, strlen(buf));

    /* 读并转发请求头 */
    char xfwd_added = 0;
    while (Rio_readlineb(&rio, buf, MAXLINE) > 0) {
        if (strcmp(buf, "\r\n") == 0) {
            /* 在空行前注入 X-Forwarded-For */
            if (!xfwd_added) {
                char xfwd[MAXLINE];
                snprintf(xfwd, sizeof(xfwd), "X-Forwarded-For: %s\r\n", client_ip);
                Rio_writen(server_fd, xfwd, strlen(xfwd));
                xfwd_added = 1;
            }
            Rio_writen(server_fd, buf, strlen(buf));
            break;
        }
        /* 替换 Host: 头 */
        if (strncasecmp(buf, "Host:", 5) == 0) {
            char new_host[MAXLINE];
            snprintf(new_host, sizeof(new_host), "Host: %s:%d\r\n",
                     backend->host, backend->port);
            Rio_writen(server_fd, new_host, strlen(new_host));
        } else {
            Rio_writen(server_fd, buf, strlen(buf));
        }
    }

    /* 打印请求日志 */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_info);
    printf("[%s] %s -> %s:%d %s\n",
           timebuf, client_ip, backend->host, backend->port, uri_for_log);
    fflush(stdout);

    atomic_fetch_add(&backend->request_count, 1);

    /* 双向中继：转发后端响应给客户端 */
    relay_data(client_fd, server_fd);
    Close(server_fd);
}

/* ---- 工作线程 ---- */
/* 带 client_ip 的工作队列版本：用结构体包装 fd+ip */
/* 为保持 sbuf_t 接口兼容（存 int），我们只存 fd，IP 通过 getpeername 获取 */
static void *proxy_worker_v2(void *vargp) {
    Pthread_detach(Pthread_self());
    while (1) {
        int connfd = sbuf_remove(&g_sbuf);
        /* 获取客户端 IP */
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        char client_ip[INET6_ADDRSTRLEN] = "unknown";
        if (getpeername(connfd, (SA *)&addr, &addrlen) == 0) {
            if (addr.ss_family == AF_INET) {
                struct sockaddr_in *s = (struct sockaddr_in *)&addr;
                inet_ntop(AF_INET, &s->sin_addr, client_ip, sizeof(client_ip));
            } else if (addr.ss_family == AF_INET6) {
                struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
                inet_ntop(AF_INET6, &s->sin6_addr, client_ip, sizeof(client_ip));
            }
        }
        forward_request(connfd, client_ip);
        Close(connfd);
    }
    return NULL;
}

/* ---- 健康检查线程 ---- */
static void *health_check_thread(void *vargp) {
    proxy_config_t *cfg = (proxy_config_t *)vargp;
    while (1) {
        sleep(3);
        for (int i = 0; i < cfg->count; i++) {
            int fd = open_clientfd(cfg->backends[i].host, cfg->backends[i].port);
            if (fd < 0) {
                if (atomic_load(&cfg->backends[i].is_alive)) {
                    printf("[health] backend %s:%d is DOWN\n",
                           cfg->backends[i].host, cfg->backends[i].port);
                    fflush(stdout);
                }
                atomic_store(&cfg->backends[i].is_alive, 0);
            } else {
                if (!atomic_load(&cfg->backends[i].is_alive)) {
                    printf("[health] backend %s:%d is UP\n",
                           cfg->backends[i].host, cfg->backends[i].port);
                    fflush(stdout);
                }
                atomic_store(&cfg->backends[i].is_alive, 1);
                close(fd);
            }
        }
    }
    return NULL;
}

/* ---- 解析 "host:port" 字符串 ---- */
static int parse_backend(const char *str, char *host_out, int *port_out) {
    char tmp[512];
    strncpy(tmp, str, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *colon = strrchr(tmp, ':');
    if (!colon) return -1;
    *colon = '\0';
    *port_out = atoi(colon + 1);
    if (*port_out <= 0 || *port_out > 65535) return -1;
    strncpy(host_out, tmp, 255);
    host_out[255] = '\0';
    return 0;
}

/* ---- main ---- */
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <listen-port> <host:port> [host:port ...]\n", argv[0]);
        exit(1);
    }

    int listen_port = atoi(argv[1]);
    if (listen_port <= 0) {
        fprintf(stderr, "invalid listen port\n");
        exit(1);
    }

    /* 初始化后端列表 */
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.count = 0;
    atomic_init(&g_cfg.rr_index, 0);

    int backend_count = argc - 2;
    if (backend_count > MAX_BACKENDS) backend_count = MAX_BACKENDS;

    for (int i = 0; i < backend_count; i++) {
        char host[256];
        int  port;
        if (parse_backend(argv[2 + i], host, &port) < 0) {
            fprintf(stderr, "invalid backend: %s\n", argv[2 + i]);
            exit(1);
        }
        strncpy(g_cfg.backends[i].host, host, 255);
        g_cfg.backends[i].port = port;
        atomic_init(&g_cfg.backends[i].request_count, 0);
        atomic_init(&g_cfg.backends[i].is_alive, 1); /* 初始假设存活 */
        g_cfg.count++;
    }

    printf("TinyProxy listening on port %d, backends:\n", listen_port);
    for (int i = 0; i < g_cfg.count; i++) {
        printf("  [%d] %s:%d\n", i, g_cfg.backends[i].host, g_cfg.backends[i].port);
    }
    fflush(stdout);

    /* 忽略 SIGPIPE */
    Signal(SIGPIPE, SIG_IGN);

    /* 初始化线程安全队列 */
    sbuf_init(&g_sbuf, PROXY_SBUF_SIZE);

    /* 启动健康检查线程 */
    pthread_t hc_tid;
    Pthread_create(&hc_tid, NULL, health_check_thread, &g_cfg);

    /* 启动工作线程池 */
    pthread_t tid_arr[PROXY_THREAD_COUNT];
    for (int i = 0; i < PROXY_THREAD_COUNT; i++) {
        Pthread_create(&tid_arr[i], NULL, proxy_worker_v2, NULL);
    }

    /* 主循环：接受连接并入队 */
    int listenfd = Open_listenfd(listen_port);
    struct sockaddr_storage clientaddr;
    socklen_t clientlen;

    while (1) {
        clientlen = sizeof(clientaddr);
        int connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        sbuf_insert(&g_sbuf, connfd);
    }

    return 0;
}
