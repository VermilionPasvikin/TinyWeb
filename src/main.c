#include "csapp.h"
#include "sio.h"
#include "sbuf.h"
#include "admin.h"
#include <libgen.h>   /* dirname() */
#define THREAD_COUNT 8
#define SBUF_SIZE 32

sbuf_t sbuf;
char g_root[MAXLINE]; /* 站点根目录，默认为 ./www */
#ifdef __APPLE__
sem_t *terminal_mutex;
#else
sem_t terminal_mutex;
#endif

void doit(int fd);
void admin_handle_standalone(int fd);
void read_requesthdrs(rio_t *rp, long *content_length, char *content_type);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, off_t filesize, int http11, int is_head);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs,
                   const char *method, long content_length,
                   const char *content_type, int http11);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg, int http11);
void *thread_worker(void *vargp);
void sigpipe_handler(int sig);
void sigchld_handler(int sig);

void sigpipe_handler(int sig)
{
    /* 简单地忽略SIGPIPE信号 */
    return;
}

void sigterm_handler(int sig)
{
    /* 清理资源并退出 */
    sbuf_deinit(&sbuf);
    
#ifdef __APPLE__
    /* macOS平台关闭并删除命名信号量 */
    Sem_close(terminal_mutex);
    Sem_unlink("/tinyweb_terminal_mutex");
#endif
    
    exit(0);
}

void sigchld_handler(int sig)
{
    int olderrno = errno;
    sigset_t mask, prev_mask;
    pid_t pid;
    sigfillset(&mask);
    sigprocmask(SIG_BLOCK, &mask, &prev_mask);

    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0)
    {
        sio_puts("Reaped a childprocess, pid: ");
        sio_putl(pid);
        sio_puts(".\n\n");
    }

    sigprocmask(SIG_SETMASK, &prev_mask, NULL);
    errno = olderrno;
}

int main(int argc, char **argv)
{
    int listenfd, connfd;
    char hostname[MAXLINE], port_s[MAXLINE];
    int port_t, admin_port = 0;
    int adminfd = -1;
    pthread_t tid_arr[THREAD_COUNT];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

    /* 自动切换到可执行文件所在目录，保证 www/ 相对路径始终有效 */
#ifdef __APPLE__
    {
        char exe_path[MAXLINE];
        uint32_t size = sizeof(exe_path);
        if (_NSGetExecutablePath(exe_path, &size) == 0) {
            char *dir = dirname(exe_path);
            if (chdir(dir) != 0)
                perror("chdir to exe directory failed");
        }
    }
#else
    {
        char exe_path[MAXLINE];
        ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (len > 0) {
            exe_path[len] = '\0';
            char *dir = dirname(exe_path);
            if (chdir(dir) != 0)
                perror("chdir to exe directory failed");
        }
    }
#endif

    Signal(SIGCHLD, sigchld_handler);
    Signal(SIGPIPE, sigpipe_handler);
    Signal(SIGINT, sigterm_handler);  /* 捕获Ctrl+C信号 */
    Signal(SIGTERM, sigterm_handler); /* 捕获终止信号 */
    Signal(SIGQUIT, sigterm_handler); /* 捕获退出信号 */
#ifdef __APPLE__
    Sem_init(&terminal_mutex, "/tinyweb_terminal_mutex", 0, 1);
#else
    Sem_init(&terminal_mutex, 0, 1);
#endif

    /* 解析命令行参数：TinyWeb <port> [--admin-port <port>] [--root <dir>] */
    if (argc < 2) {
        fprintf(stderr, "usage: %s <port> [--admin-port <port>] [--root <dir>]\n", argv[0]);
        exit(1);
    }

    if(sscanf(argv[1], "%d", &port_t) != 1) {
        fprintf(stderr, "invalid port\n");
        exit(1);
    }

    /* 解析 --root 和 --admin-port 参数 */
    strcpy(g_root, "www");
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--root") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--root requires an argument\n"); exit(1); }
            strncpy(g_root, argv[i+1], MAXLINE - 1);
            g_root[MAXLINE - 1] = '\0';
            int rlen = strlen(g_root);
            if (rlen > 1 && g_root[rlen-1] == '/')
                g_root[rlen-1] = '\0';
            i++;
        } else if (strcmp(argv[i], "--admin-port") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--admin-port requires an argument\n"); exit(1); }
            if (sscanf(argv[i+1], "%d", &admin_port) != 1 || admin_port <= 0) {
                fprintf(stderr, "invalid admin port\n");
                exit(1);
            }
            i++;
        }
    }

    printf("TinyWeb starting on port %d, serving from: %s\n", port_t, g_root);
    if (admin_port > 0)
        printf("Admin interface on 127.0.0.1:%d\n", admin_port);

    /* 初始化统计数据 */
    g_stats.start_time = time(NULL);
    g_stats.last_window_time = g_stats.start_time;

    listenfd = Open_listenfd(port_t);

    /* 管理端口仅绑定 127.0.0.1 */
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

    sbuf_init(&sbuf, SBUF_SIZE);

    for (size_t i = 0; i < THREAD_COUNT; i++)
    {
        Pthread_create(&(tid_arr[i]), NULL, thread_worker, NULL);
    }

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
            connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
            Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE,
                        port_s, MAXLINE, 0);
            atomic_fetch_add(&g_stats.active_connections, 1);
            P(&terminal_mutex);
            printf("Accepted connection from (%s, %s)\n", hostname, port_s);
            V(&terminal_mutex);
            sbuf_insert(&sbuf, connfd);
        }

        if (adminfd >= 0 && FD_ISSET(adminfd, &readfds)) {
            clientlen = sizeof(clientaddr);
            connfd = Accept(adminfd, (SA *)&clientaddr, &clientlen);
            /* ~connfd 为负数，用于标识管理连接 */
            sbuf_insert(&sbuf, ~connfd);
        }
    }
    return 0;
}

void *thread_worker(void *vargp)
{
    Pthread_detach(Pthread_self());
    while (1)
    {
        int raw = sbuf_remove(&sbuf);
        int is_admin = (raw < 0);
        int connfd = is_admin ? ~raw : raw;
        if (is_admin) {
            admin_handle_standalone(connfd);
        } else {
            doit(connfd);
            atomic_fetch_sub(&g_stats.active_connections, 1);
        }
        Close(connfd);
    }
}

/* 管理端口专用入口：读请求行后交给 admin_handle() */
void admin_handle_standalone(int fd)
{
    char buf[MAXLINE] = {0}, method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    rio_t rio;

    Rio_readinitb(&rio, fd);
    if (Rio_readlineb(&rio, buf, MAXLINE) <= 0)
        return;

    sscanf(buf, "%8191s %8191s %8191s", method, uri, version);
    admin_handle(&rio, fd, method, uri);
    atomic_fetch_add(&g_stats.total_requests, 1);
    stats_update_window();
}

void doit(int fd)
{
    int is_static;
    struct stat sbuf;
    char buf[MAXLINE] = {0}, method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    char content_type[MAXLINE];
    long content_length = 0;
    rio_t rio;

    Rio_readinitb(&rio, fd);
    if(Rio_readlineb(&rio, buf, MAXLINE) <= 0)
        return;

    P(&terminal_mutex);
    printf("Request headers:\n");
    printf("%s", buf);
    V(&terminal_mutex);

    sscanf(buf, "%8191s %8191s %8191s", method, uri, version);

    int http11 = (strncmp(version, "HTTP/1.1", 8) == 0);
    int is_get  = (strcasecmp(method, "GET")  == 0);
    int is_post = (strcasecmp(method, "POST") == 0);
    int is_head = (strcasecmp(method, "HEAD") == 0);

    if (!is_get && !is_post && !is_head) {
        clienterror(fd, method, "501", "Not Implemented",
                    "Tiny does not implement this method", http11);
        return;
    }

    content_type[0] = '\0';
    read_requesthdrs(&rio, &content_length, content_type);

    is_static = parse_uri(uri, filename, cgiargs);
    if(is_static == -1)
    {
        clienterror(fd, filename, "400", "Bad Request",
                     "Illegal parameters in URI.", http11);
        return;
    }

    if (stat(filename, &sbuf) < 0) {
        clienterror(fd, filename, "404", "Not found",
                    "Tiny couldn't find this file", http11);
        return;
    }

    if (is_static) {
        if (!is_get && !is_head) {
            clienterror(fd, method, "405", "Method Not Allowed",
                        "Static files only support GET and HEAD", http11);
            return;
        }
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
            clienterror(fd, filename, "403", "Forbidden",
                        "Tiny couldn't read the file", http11);
            return;
        }
        serve_static(fd, filename, sbuf.st_size, http11, is_head);
    }
    else {
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) {
            clienterror(fd, filename, "403", "Forbidden",
                        "Tiny couldn't run the CGI program", http11);
            return;
        }
        serve_dynamic(fd, filename, cgiargs, method, content_length,
                      content_type, http11);
    }
}

void read_requesthdrs(rio_t *rp, long *content_length, char *content_type)
{
    char buf[MAXLINE] = {0};
    ssize_t n;
    while ((n = Rio_readlineb(rp, buf, MAXLINE)) > 0) {
        if (strcmp(buf, "\r\n") == 0) break;
        P(&terminal_mutex);
        printf("%s", buf);
        fflush(stdout);
        V(&terminal_mutex);
        if (strncasecmp(buf, "Content-Length:", 15) == 0)
            *content_length = atol(buf + 15);
        else if (strncasecmp(buf, "Content-Type:", 13) == 0)
            sscanf(buf + 13, " %8191s", content_type);
    }
    return;
}

int parse_uri(char *uri, char *filename, char *cgiargs)
{
    char *ptr;

    if (strstr(uri, "..") != NULL) {
        return -1;
    }

    size_t root_len = strlen(g_root);
    size_t uri_len  = strlen(uri);
    /* 防止 g_root + uri + "index.html" 超出 MAXLINE */
    if (root_len + uri_len + 12 >= MAXLINE)
        return -1;

    if (!strstr(uri, "cgi-bin")) {
        cgiargs[0] = '\0';
        snprintf(filename, MAXLINE, "%s%s", g_root, uri);
        if (uri_len > 0 && uri[uri_len - 1] == '/')
            strncat(filename, "index.html", MAXLINE - strlen(filename) - 1);
        return 1;
    }
    else {
        ptr = strchr(uri, '?');
        if (ptr) {
            strncpy(cgiargs, ptr + 1, MAXLINE - 1);
            cgiargs[MAXLINE - 1] = '\0';
            *ptr = '\0';
        }
        else
            cgiargs[0] = '\0';
        snprintf(filename, MAXLINE, "%s%s", g_root, uri);
        return 0;
    }
}

void serve_static(int fd, char *filename, off_t filesize, int http11, int is_head)
{
    int srcfd;
    char *srcp, filetype[MAXLINE], buf[MAXLINE];
    int n = 0;

    get_filetype(filename, filetype);

    n += snprintf(buf + n, MAXLINE - n, "%s 200 OK\r\n",
                  http11 ? "HTTP/1.1" : "HTTP/1.0");
    n += snprintf(buf + n, MAXLINE - n, "Server: Tiny Web Server\r\n");
    n += snprintf(buf + n, MAXLINE - n, "Content-Length: %lld\r\n", (long long)filesize);
    n += snprintf(buf + n, MAXLINE - n, "Content-Type: %s\r\n", filetype);
    if (http11)
        n += snprintf(buf + n, MAXLINE - n, "Connection: close\r\n");
    n += snprintf(buf + n, MAXLINE - n, "\r\n");
    Rio_writen(fd, buf, n);

    if (is_head) {
        atomic_fetch_add(&g_stats.bytes_sent, n);
        return;
    }

    srcfd = Open(filename, O_RDONLY, 0);
    if (filesize < 1024 * 1024) {
        char *filebuf = (char *)Malloc(filesize);
        Read(srcfd, filebuf, filesize);
        Close(srcfd);
        Rio_writen(fd, filebuf, filesize);
        Free(filebuf);
    } else {
        srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
        Close(srcfd);
        Rio_writen(fd, srcp, filesize);
        Munmap(srcp, filesize);
    }
    atomic_fetch_add(&g_stats.bytes_sent, filesize);
}

void get_filetype(char *filename, char *filetype)
{
    if (strstr(filename, ".html") || strstr(filename, ".htm"))
        strcpy(filetype, "text/html");
    else if (strstr(filename, ".css"))
        strcpy(filetype, "text/css");
    else if (strstr(filename, ".js"))
        strcpy(filetype, "application/javascript");
    else if (strstr(filename, ".json"))
        strcpy(filetype, "application/json");
    else if (strstr(filename, ".xml"))
        strcpy(filetype, "application/xml");
    else if (strstr(filename, ".gif"))
        strcpy(filetype, "image/gif");
    else if (strstr(filename, ".jpg") || strstr(filename, ".jpeg") || strstr(filename, ".jpe"))
        strcpy(filetype, "image/jpeg");
    else if (strstr(filename, ".png"))
        strcpy(filetype, "image/png");
    else if (strstr(filename, ".webp"))
        strcpy(filetype, "image/webp");
    else if (strstr(filename, ".svg"))
        strcpy(filetype, "image/svg+xml");
    else if (strstr(filename, ".ico"))
        strcpy(filetype, "image/x-icon");
    else if (strstr(filename, ".mp4"))
        strcpy(filetype, "video/mp4");
    else if (strstr(filename, ".webm"))
        strcpy(filetype, "video/webm");
    else if (strstr(filename, ".mpg") || strstr(filename, ".mpeg") || strstr(filename, ".mpe"))
        strcpy(filetype, "video/mpeg");
    else if (strstr(filename, ".mp3"))
        strcpy(filetype, "audio/mpeg");
    else if (strstr(filename, ".wav"))
        strcpy(filetype, "audio/wav");
    else if (strstr(filename, ".woff2"))
        strcpy(filetype, "font/woff2");
    else if (strstr(filename, ".woff"))
        strcpy(filetype, "font/woff");
    else if (strstr(filename, ".ttf"))
        strcpy(filetype, "font/ttf");
    else if (strstr(filename, ".pdf"))
        strcpy(filetype, "application/pdf");
    else
        strcpy(filetype, "application/octet-stream");
}

void serve_dynamic(int fd, char *filename, char *cgiargs,
                   const char *method, long content_length,
                   const char *content_type, int http11)
{
    char buf[MAXLINE], *emptylist[] = { NULL };
    char cl_str[32];

    snprintf(buf, sizeof(buf), "%s 200 OK\r\n", http11 ? "HTTP/1.1" : "HTTP/1.0");
    Rio_writen(fd, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));
    if (http11) {
        snprintf(buf, sizeof(buf), "Connection: close\r\n");
        Rio_writen(fd, buf, strlen(buf));
    }

    atomic_fetch_add(&g_stats.total_requests, 1);
    stats_update_window();

    if (Fork() == 0) {
        setenv("QUERY_STRING",   cgiargs,                1);
        setenv("REQUEST_METHOD", method,                 1);
        snprintf(cl_str, sizeof(cl_str), "%ld", content_length);
        setenv("CONTENT_LENGTH", cl_str,                 1);
        setenv("CONTENT_TYPE",   content_type[0] ? content_type : "application/octet-stream", 1);
        Dup2(fd, STDOUT_FILENO);
        if (strcasecmp(method, "POST") == 0)
            Dup2(fd, STDIN_FILENO);
        Execve(filename, emptylist, environ);
    }
}

void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg, int http11)
{
    char buf[MAXLINE], body[MAXBUF];
    int bn = 0;

    bn += snprintf(body + bn, MAXBUF - bn, "<html><title>Tiny Error</title>");
    bn += snprintf(body + bn, MAXBUF - bn, "<body bgcolor=\"ffffff\">\r\n");
    bn += snprintf(body + bn, MAXBUF - bn, "%s: %s\r\n", errnum, shortmsg);
    bn += snprintf(body + bn, MAXBUF - bn, "<p>%s: %s\r\n", longmsg, cause);
    bn += snprintf(body + bn, MAXBUF - bn, "<hr><em>The Tiny Web server</em>\r\n");

    snprintf(buf, sizeof(buf), "%s %s %s\r\n",
             http11 ? "HTTP/1.1" : "HTTP/1.0", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "Content-Type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "Content-Length: %d\r\n", bn);
    Rio_writen(fd, buf, strlen(buf));
    if (http11) {
        snprintf(buf, sizeof(buf), "Connection: close\r\n");
        Rio_writen(fd, buf, strlen(buf));
    }
    snprintf(buf, sizeof(buf), "\r\n");
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, bn);
    atomic_fetch_add(&g_stats.total_errors, 1);
    atomic_fetch_add(&g_stats.total_requests, 1);
    stats_update_window();
}