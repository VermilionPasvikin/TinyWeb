#include "csapp.h"
#include "sbuf.h"
#include "proxy_config.h"
#include "admin.h"
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
static server_stats_t  g_proxy_stats;

/* 管理面板前向声明 */
static void proxy_admin_handle(int fd);
static void proxy_stats_update_window(void);

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

/* ---- 双向中继（select + 超时），返回从后端收到的字节数 ---- */
static long relay_data(int client_fd, int server_fd) {
    fd_set fds;
    char buf[BUF_SIZE];
    struct timeval tv;
    int maxfd = (client_fd > server_fd ? client_fd : server_fd) + 1;
    long bytes_from_backend = 0;

    while (1) {
        tv.tv_sec  = RELAY_TIMEOUT_SEC;
        tv.tv_usec = 0;
        FD_ZERO(&fds);
        FD_SET(client_fd, &fds);
        FD_SET(server_fd, &fds);

        int ret = select(maxfd, &fds, NULL, NULL, &tv);
        if (ret == 0) break;          /* 超时：关闭连接 */
        if (ret < 0) {
            if (errno == EINTR) continue;  /* 信号中断：重试 */
            break;                         /* 其他错误：关闭连接 */
        }

        if (FD_ISSET(client_fd, &fds)) {
            ssize_t n = read(client_fd, buf, sizeof(buf));
            if (n <= 0) break;
            Rio_writen(server_fd, buf, n);
        }
        if (FD_ISSET(server_fd, &fds)) {
            ssize_t n = read(server_fd, buf, sizeof(buf));
            if (n <= 0) break;
            Rio_writen(client_fd, buf, n);
            bytes_from_backend += n;
        }
    }
    return bytes_from_backend;
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
        atomic_fetch_add(&g_proxy_stats.total_errors, 1);
        atomic_fetch_add(&g_proxy_stats.total_requests, 1);
        proxy_stats_update_window();
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
        atomic_fetch_add(&g_proxy_stats.total_errors, 1);
        atomic_fetch_add(&g_proxy_stats.total_requests, 1);
        proxy_stats_update_window();
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
    sscanf(buf, "%*s %8191s", uri_for_log);
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
    long bytes = relay_data(client_fd, server_fd);
    Close(server_fd);

    atomic_fetch_add(&g_proxy_stats.total_requests, 1);
    atomic_fetch_add(&g_proxy_stats.bytes_sent, bytes);
    proxy_stats_update_window();
}

/* ---- 工作线程 ---- */
/* 带 client_ip 的工作队列版本：用结构体包装 fd+ip */
/* 为保持 sbuf_t 接口兼容（存 int），我们只存 fd，IP 通过 getpeername 获取 */
static void *proxy_worker_v2(void *vargp) {
    Pthread_detach(Pthread_self());
    while (1) {
        int raw = sbuf_remove(&g_sbuf);
        int is_admin = (raw < 0);
        int connfd = is_admin ? ~raw : raw;

        if (is_admin) {
            proxy_admin_handle(connfd);
            Close(connfd);
            continue;
        }

        atomic_fetch_add(&g_proxy_stats.active_connections, 1);

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
        atomic_fetch_sub(&g_proxy_stats.active_connections, 1);
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
        fprintf(stderr, "usage: %s <listen-port> [--admin-port <port>] <host:port> [host:port ...]\n", argv[0]);
        exit(1);
    }

    int listen_port = atoi(argv[1]);
    if (listen_port <= 0) {
        fprintf(stderr, "invalid listen port\n");
        exit(1);
    }

    /* 解析 --admin-port（可出现在后端列表之前或之后） */
    int admin_port = 0;
    int backend_start = 2;

    /* 先扫描一遍找 --admin-port，允许它出现在任意位置 */
    for (int i = 2; i < argc - 1; i++) {
        if (strcmp(argv[i], "--admin-port") == 0) {
            if (sscanf(argv[i+1], "%d", &admin_port) != 1 || admin_port <= 0) {
                fprintf(stderr, "invalid admin port\n");
                exit(1);
            }
            /* 将 --admin-port <val> 从参数列表中逻辑跳过 */
            backend_start = 2;
            break;
        }
    }

    /* 重新收集后端参数（跳过 --admin-port 及其值） */
    int backend_argc = 0;
    char *backend_argv[MAX_BACKENDS + 1];
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--admin-port") == 0) { i++; continue; }
        if (backend_argc < MAX_BACKENDS)
            backend_argv[backend_argc++] = argv[i];
    }

    if (backend_argc == 0) {
        fprintf(stderr, "no backends specified\n");
        exit(1);
    }

    /* 初始化后端列表 */
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.count = 0;
    atomic_init(&g_cfg.rr_index, 0);

    int backend_count = backend_argc;

    for (int i = 0; i < backend_count; i++) {
        char host[256];
        int  port;
        if (parse_backend(backend_argv[i], host, &port) < 0) {
            fprintf(stderr, "invalid backend: %s\n", backend_argv[i]);
            exit(1);
        }
        strncpy(g_cfg.backends[i].host, host, 255);
        g_cfg.backends[i].port = port;
        atomic_init(&g_cfg.backends[i].request_count, 0);
        atomic_init(&g_cfg.backends[i].is_alive, 1);
        g_cfg.count++;
    }

    /* 初始化代理统计 */
    g_proxy_stats.start_time = time(NULL);
    g_proxy_stats.last_window_time = g_proxy_stats.start_time;

    printf("TinyProxy listening on port %d, backends:\n", listen_port);
    for (int i = 0; i < g_cfg.count; i++) {
        printf("  [%d] %s:%d\n", i, g_cfg.backends[i].host, g_cfg.backends[i].port);
    }
    if (admin_port > 0)
        printf("Admin interface on 127.0.0.1:%d\n", admin_port);
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

    /* 管理端口仅绑定 127.0.0.1 */
    int adminfd = -1;
    if (admin_port > 0) {
        struct sockaddr_in adminaddr;
        int optval = 1;
        adminfd = socket(AF_INET, SOCK_STREAM, 0);
        if (adminfd < 0) { perror("admin socket"); exit(1); }
        if (setsockopt(adminfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
            perror("admin setsockopt"); exit(1);
        }
        memset(&adminaddr, 0, sizeof(adminaddr));
        adminaddr.sin_family = AF_INET;
        adminaddr.sin_addr.s_addr = inet_addr("127.0.0.1");
        adminaddr.sin_port = htons((unsigned short)admin_port);
        if (bind(adminfd, (SA *)&adminaddr, sizeof(adminaddr)) < 0) {
            perror("admin bind"); exit(1);
        }
        if (listen(adminfd, LISTENQ) < 0) {
            perror("admin listen"); exit(1);
        }
    }

    /* 主循环：select 同时监听业务端口和管理端口 */
    int listenfd = Open_listenfd(listen_port);
    struct sockaddr_storage clientaddr;
    socklen_t clientlen;

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listenfd, &readfds);
        int maxfd = listenfd;
        if (adminfd >= 0) {
            FD_SET(adminfd, &readfds);
            if (adminfd > maxfd) maxfd = adminfd;
        }

        if (select(maxfd + 1, &readfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (FD_ISSET(listenfd, &readfds)) {
            clientlen = sizeof(clientaddr);
            int connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
            sbuf_insert(&g_sbuf, connfd);
        }

        if (adminfd >= 0 && FD_ISSET(adminfd, &readfds)) {
            clientlen = sizeof(clientaddr);
            int connfd = Accept(adminfd, (SA *)&clientaddr, &clientlen);
            sbuf_insert(&g_sbuf, ~connfd);
        }
    }

    return 0;
}

/* ================================================================
 * TinyProxy 管理面板
 * ================================================================ */

static void proxy_stats_update_window(void) {
    time_t now = time(NULL);
    time_t last = g_proxy_stats.last_window_time;
    if (last == 0) { g_proxy_stats.last_window_time = now; return; }
    long diff = (long)(now - last);
    if (diff <= 0) {
        int idx = atomic_load(&g_proxy_stats.window_idx);
        atomic_fetch_add(&g_proxy_stats.req_per_sec[idx], 1);
        return;
    }
    if (diff > RATE_WINDOW) diff = RATE_WINDOW;
    int cur_idx = atomic_load(&g_proxy_stats.window_idx);
    for (long i = 0; i < diff; i++) {
        cur_idx = (cur_idx + 1) % RATE_WINDOW;
        atomic_store(&g_proxy_stats.req_per_sec[cur_idx], 0);
    }
    atomic_store(&g_proxy_stats.req_per_sec[cur_idx], 1);
    atomic_store(&g_proxy_stats.window_idx, cur_idx);
    g_proxy_stats.last_window_time = now;
}

static void proxy_send_headers(int fd, const char *status,
                               const char *ctype, long clen) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n\r\n",
        status, ctype, clen);
    Rio_writen(fd, buf, strlen(buf));
}

static const char PROXY_ADMIN_HTML[] =
"<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n"
"<meta charset=\"UTF-8\">\n<title>TinyProxy 管理面板</title>\n"
"<style>\n"
"* { box-sizing:border-box; margin:0; padding:0; }\n"
"body { font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;\n"
"       background:#0f172a; color:#e2e8f0; min-height:100vh; padding:24px; }\n"
"h1 { font-size:1.5rem; font-weight:700; margin-bottom:24px;\n"
"     display:flex; align-items:center; gap:10px; }\n"
".dot { width:12px; height:12px; border-radius:50%; background:#22c55e;\n"
"       box-shadow:0 0 8px #22c55e; animation:pulse 2s infinite; }\n"
".dot.dead { background:#ef4444; box-shadow:0 0 8px #ef4444; animation:none; }\n"
"@keyframes pulse { 0%,100%{opacity:1} 50%{opacity:0.5} }\n"
".grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(200px,1fr));\n"
"        gap:16px; margin-bottom:24px; }\n"
".card { background:#1e293b; border-radius:12px; padding:20px;\n"
"        border:1px solid #334155; }\n"
".card-label { font-size:.75rem; color:#94a3b8; text-transform:uppercase;\n"
"              letter-spacing:.05em; margin-bottom:8px; }\n"
".card-value { font-size:2rem; font-weight:700; color:#f1f5f9; }\n"
".card-unit { font-size:.875rem; color:#64748b; margin-top:4px; }\n"
".chart-wrap { background:#1e293b; border-radius:12px; padding:20px;\n"
"              border:1px solid #334155; margin-bottom:24px; }\n"
".chart-title { font-size:.875rem; color:#94a3b8; margin-bottom:12px; }\n"
"canvas { width:100% !important; height:120px !important; }\n"
".backends { background:#1e293b; border-radius:12px; padding:20px;\n"
"            border:1px solid #334155; margin-bottom:24px; }\n"
".backends h2 { font-size:.875rem; color:#94a3b8; margin-bottom:12px; }\n"
"table { width:100%; border-collapse:collapse; font-size:.875rem; }\n"
"th,td { padding:8px 12px; text-align:left; border-bottom:1px solid #334155; }\n"
"th { color:#94a3b8; font-weight:600; }\n"
".alive { color:#22c55e; } .dead-text { color:#ef4444; }\n"
".actions { display:flex; gap:12px; }\n"
"button { padding:10px 20px; border:none; border-radius:8px; cursor:pointer;\n"
"         font-size:.875rem; font-weight:600; transition:opacity .2s; }\n"
"button:hover { opacity:.8; }\n"
".btn-stop { background:#ef4444; color:white; }\n"
".btn-refresh { background:#3b82f6; color:white; }\n"
".footer { margin-top:24px; font-size:.75rem; color:#475569; }\n"
"</style>\n</head>\n<body>\n"
"<h1><span id=\"dot\" class=\"dot\"></span>TinyProxy 管理面板</h1>\n"
"<div class=\"grid\">\n"
"  <div class=\"card\"><div class=\"card-label\">当前连接</div>\n"
"    <div class=\"card-value\" id=\"active\">-</div></div>\n"
"  <div class=\"card\"><div class=\"card-label\">总请求数</div>\n"
"    <div class=\"card-value\" id=\"total\">-</div></div>\n"
"  <div class=\"card\"><div class=\"card-label\">错误数</div>\n"
"    <div class=\"card-value\" id=\"errors\">-</div></div>\n"
"  <div class=\"card\"><div class=\"card-label\">运行时长</div>\n"
"    <div class=\"card-value\" id=\"uptime\">-</div>\n"
"    <div class=\"card-unit\">秒</div></div>\n"
"  <div class=\"card\"><div class=\"card-label\">已转发流量</div>\n"
"    <div class=\"card-value\" id=\"bytes\">-</div>\n"
"    <div class=\"card-unit\" id=\"bytes-unit\">bytes</div></div>\n"
"  <div class=\"card\"><div class=\"card-label\">当前 RPS</div>\n"
"    <div class=\"card-value\" id=\"rps\">-</div>\n"
"    <div class=\"card-unit\">req/s</div></div>\n"
"</div>\n"
"<div class=\"chart-wrap\">\n"
"  <div class=\"chart-title\">请求速率（近60秒）</div>\n"
"  <canvas id=\"chart\"></canvas>\n"
"</div>\n"
"<div class=\"backends\">\n"
"  <h2>后端实例</h2>\n"
"  <table><thead><tr><th>#</th><th>地址</th><th>状态</th><th>请求数</th></tr></thead>\n"
"  <tbody id=\"backends-tbody\"></tbody></table>\n"
"</div>\n"
"<div class=\"actions\">\n"
"  <button class=\"btn-refresh\" onclick=\"poll()\">立即刷新</button>\n"
"  <button class=\"btn-stop\" onclick=\"stopProxy()\">停止代理</button>\n"
"</div>\n"
"<div class=\"footer\" id=\"last-update\">等待数据...</div>\n"
"<script>\n"
"const canvas=document.getElementById('chart');\n"
"const ctx=canvas.getContext('2d');\n"
"function fmtBytes(b){\n"
"  if(b<1024)return[b.toFixed(0),'bytes'];\n"
"  if(b<1048576)return[(b/1024).toFixed(1),'KB'];\n"
"  if(b<1073741824)return[(b/1048576).toFixed(1),'MB'];\n"
"  return[(b/1073741824).toFixed(2),'GB'];}\n"
"function drawChart(data){\n"
"  const w=canvas.parentElement.clientWidth-40,h=120;\n"
"  canvas.width=w;canvas.height=h;\n"
"  const max=Math.max(...data,1);\n"
"  ctx.clearRect(0,0,w,h);\n"
"  ctx.fillStyle='#0f172a';ctx.fillRect(0,0,w,h);\n"
"  const step=w/(data.length-1);\n"
"  ctx.beginPath();ctx.strokeStyle='#3b82f6';ctx.lineWidth=2;\n"
"  data.forEach((v,i)=>{\n"
"    const x=i*step,y=h-(v/max)*(h-10)-5;\n"
"    i===0?ctx.moveTo(x,y):ctx.lineTo(x,y);});\n"
"  ctx.stroke();\n"
"  ctx.lineTo(w,h);ctx.lineTo(0,h);\n"
"  ctx.fillStyle='rgba(59,130,246,0.15)';ctx.fill();}\n"
"async function poll(){\n"
"  try{\n"
"    const[s,m,b]=await Promise.all([\n"
"      fetch('/__admin/status').then(r=>r.json()),\n"
"      fetch('/__admin/metrics').then(r=>r.json()),\n"
"      fetch('/__admin/backends').then(r=>r.json())]);\n"
"    document.getElementById('active').textContent=s.active_connections;\n"
"    document.getElementById('total').textContent=s.total_requests;\n"
"    document.getElementById('errors').textContent=s.total_errors;\n"
"    document.getElementById('uptime').textContent=s.uptime_seconds;\n"
"    const[bv,bu]=fmtBytes(m.bytes_sent);\n"
"    document.getElementById('bytes').textContent=bv;\n"
"    document.getElementById('bytes-unit').textContent=bu;\n"
"    const rpsArr=m.req_per_sec;\n"
"    const curRps=rpsArr?rpsArr[rpsArr.length-1]||0:0;\n"
"    document.getElementById('rps').textContent=curRps;\n"
"    if(rpsArr)drawChart(rpsArr);\n"
"    const tbody=document.getElementById('backends-tbody');\n"
"    tbody.innerHTML='';\n"
"    (b.backends||[]).forEach((be,i)=>{\n"
"      tbody.innerHTML+=`<tr><td>${i}</td><td>${be.host}:${be.port}</td>`+\n"
"        `<td class=\"${be.is_alive?'alive':'dead-text'}\">${be.is_alive?'UP':'DOWN'}</td>`+\n"
"        `<td>${be.request_count}</td></tr>`;});\n"
"    document.getElementById('last-update').textContent='最后更新: '+new Date().toLocaleTimeString();\n"
"  }catch(e){\n"
"    document.getElementById('dot').className='dot dead';\n"
"    document.getElementById('last-update').textContent='连接失败: '+e.message;}}\n"
"async function stopProxy(){\n"
"  if(!confirm('确认停止代理？'))return;\n"
"  await fetch('/__admin/stop',{method:'POST'});\n"
"  document.getElementById('dot').className='dot dead';\n"
"  document.getElementById('last-update').textContent='代理已停止';}\n"
"poll();setInterval(poll,2000);\n"
"</script>\n</body>\n</html>\n";

static void handle_proxy_index(int fd) {
    long len = (long)strlen(PROXY_ADMIN_HTML);
    proxy_send_headers(fd, "200 OK", "text/html; charset=utf-8", len);
    Rio_writen(fd, (void *)PROXY_ADMIN_HTML, len);
}

static void handle_proxy_status(int fd) {
    time_t now = time(NULL);
    long uptime = (long)(now - g_proxy_stats.start_time);
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"uptime_seconds\":%ld,\"active_connections\":%d,"
        "\"total_requests\":%ld,\"total_errors\":%ld}",
        uptime,
        atomic_load(&g_proxy_stats.active_connections),
        atomic_load(&g_proxy_stats.total_requests),
        atomic_load(&g_proxy_stats.total_errors));
    proxy_send_headers(fd, "200 OK", "application/json", n);
    Rio_writen(fd, buf, n);
}

static void handle_proxy_metrics(int fd) {
    char buf[2048];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{\"bytes_sent\":%ld,\"req_per_sec\":[",
        atomic_load(&g_proxy_stats.bytes_sent));
    int cur = atomic_load(&g_proxy_stats.window_idx);
    for (int i = 0; i < RATE_WINDOW; i++) {
        int idx = (cur + 1 + i) % RATE_WINDOW;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%ld%s",
            atomic_load(&g_proxy_stats.req_per_sec[idx]),
            (i < RATE_WINDOW - 1) ? "," : "");
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    proxy_send_headers(fd, "200 OK", "application/json", pos);
    Rio_writen(fd, buf, pos);
}

static void handle_proxy_backends(int fd) {
    char buf[4096];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"backends\":[");
    for (int i = 0; i < g_cfg.count; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"host\":\"%s\",\"port\":%d,\"is_alive\":%d,\"request_count\":%ld}",
            i > 0 ? "," : "",
            g_cfg.backends[i].host,
            g_cfg.backends[i].port,
            atomic_load(&g_cfg.backends[i].is_alive),
            atomic_load(&g_cfg.backends[i].request_count));
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    proxy_send_headers(fd, "200 OK", "application/json", pos);
    Rio_writen(fd, buf, pos);
}

static void handle_proxy_stop(int fd) {
    const char *body = "{\"status\":\"stopping\"}";
    proxy_send_headers(fd, "200 OK", "application/json", (long)strlen(body));
    Rio_writen(fd, (void *)body, strlen(body));
    raise(SIGTERM);
}

static void proxy_admin_handle(int fd) {
    char buf[MAXLINE] = {0}, method[MAXLINE], uri[MAXLINE];
    rio_t rio;

    Rio_readinitb(&rio, fd);
    if (Rio_readlineb(&rio, buf, MAXLINE) <= 0) return;
    sscanf(buf, "%8191s %8191s", method, uri);

    /* 读完剩余请求头 */
    while (Rio_readlineb(&rio, buf, MAXLINE) > 0) {
        if (strcmp(buf, "\r\n") == 0) break;
    }

    if (strcmp(uri, "/__admin/") == 0 || strcmp(uri, "/__admin") == 0) {
        handle_proxy_index(fd);
    } else if (strcmp(uri, "/__admin/status") == 0) {
        handle_proxy_status(fd);
    } else if (strcmp(uri, "/__admin/metrics") == 0) {
        handle_proxy_metrics(fd);
    } else if (strcmp(uri, "/__admin/backends") == 0) {
        handle_proxy_backends(fd);
    } else if (strcmp(uri, "/__admin/stop") == 0 &&
               strcasecmp(method, "POST") == 0) {
        handle_proxy_stop(fd);
    } else {
        const char *body = "404 Not Found";
        proxy_send_headers(fd, "404 Not Found", "text/plain", (long)strlen(body));
        Rio_writen(fd, (void *)body, strlen(body));
    }
}
