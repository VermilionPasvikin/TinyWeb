#include "csapp.h"
#include "sio.h"
#include "sbuf.h"
#define THREAD_COUNT 8
#define SBUF_SIZE 32

sbuf_t sbuf;
#ifdef __APPLE__
sem_t *terminal_mutex;
#else
sem_t terminal_mutex;
#endif

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
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

    while (1)
    {
        pid = waitpid(-1, NULL, WNOHANG);
        sigprocmask(SIG_BLOCK, &mask, &prev_mask);
        if (pid > 0)
        {
            sio_puts("Reaped a childprocess, pid: ");
            sio_putl(pid);
            sio_puts(".\n\n");
        }
        else if (pid < 0)
        {
            if (errno == EINTR)
                continue;
            else if(errno == ECHILD)
                break;
            else
            {
                sio_error("waitpid_error, errno: ");
                sio_putl(errno);
                sio_error(".\n\n");
                break;
            }
        }
        sigprocmask(SIG_SETMASK, &prev_mask, NULL);
    }
    
    errno = olderrno;
}

int main(int argc, char **argv)
{
    int listenfd, connfd;
    char hostname[MAXLINE], port_s[MAXLINE];
    int port_t;
    pthread_t tid_arr[THREAD_COUNT];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

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

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    if(sscanf(argv[1], "%d", &port_t) != 1) {
        fprintf(stderr, "invalid port\n");
        exit(1);
    }

    listenfd = Open_listenfd(port_t);

    sbuf_init(&sbuf, SBUF_SIZE);

    for (size_t i = 0; i < THREAD_COUNT; i++)
    {
        Pthread_create(&(tid_arr[i]), NULL, thread_worker, NULL);
    }

    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE,
                    port_s, MAXLINE, 0);

        P(&terminal_mutex);
        printf("Accepted connection from (%s, %s)\n", hostname, port_s);
        V(&terminal_mutex);

        sbuf_insert(&sbuf,connfd);
    }
    return 0;
}

void *thread_worker(void *vargp)
{
    Pthread_detach(Pthread_self());
    while (1)
    {
        int connfd = sbuf_remove(&sbuf);
        doit(connfd);
        Close(connfd); 
    }
}

void doit(int fd) 
{
    int is_static;
    struct stat sbuf;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    rio_t rio;

    Rio_readinitb(&rio, fd);
    Rio_readlineb(&rio, buf, MAXLINE);

    P(&terminal_mutex);
    printf("Request headers:\n");
    printf("%s", buf);
    V(&terminal_mutex);

    sscanf(buf, "%s %s %s", method, uri, version);
    if (strcasecmp(method, "GET")) { 
        clienterror(fd, method, "501", "Not Implemented",
                    "Tiny does not implement this method");
        return;
    }
    read_requesthdrs(&rio);

    is_static = parse_uri(uri, filename, cgiargs);
    if (stat(filename, &sbuf) < 0) {
        clienterror(fd, filename, "404", "Not found",
                    "Tiny couldn't find this file");
        return;
    }

    if (is_static) { 
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
            clienterror(fd, filename, "403", "Forbidden",
                        "Tiny couldn't read the file");
            return;
        }
        serve_static(fd, filename, sbuf.st_size);
    }
    else { 
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) {
            clienterror(fd, filename, "403", "Forbidden",
                        "Tiny couldn't run the CGI program");
            return;
        }
        serve_dynamic(fd, filename, cgiargs);
    }
}

void read_requesthdrs(rio_t *rp) 
{
    char buf[MAXLINE];

    Rio_readlineb(rp, buf, MAXLINE);
    while (strcmp(buf, "\r\n")) {
        Rio_readlineb(rp, buf, MAXLINE);
        P(&terminal_mutex);
        printf("%s", buf);
        fflush(stdout);
        V(&terminal_mutex);
    }
    return;
}

int parse_uri(char *uri, char *filename, char *cgiargs) 
{
    char *ptr;

    // 检查是否包含目录遍历序列
    if (strstr(uri, "..") != NULL) {
        return -1; // 拒绝请求
    }

    if (!strstr(uri, "cgi-bin")) { 
        strcpy(cgiargs, "");
        strcpy(filename, ".");
        strcat(filename, uri);
        if (uri[strlen(uri)-1] == '/') 
            strcat(filename, "index.html");
        return 1;
    }
    else { 
        ptr = index(uri, '?');
        if (ptr) {
            strcpy(cgiargs, ptr+1);
            *ptr = '\0';
        }
        else
            strcpy(cgiargs, "");
        strcpy(filename, ".");
        strcat(filename, uri);
        return 0;
    }
}

void serve_static(int fd, char *filename, int filesize) 
{
    int srcfd;
    char *srcp, filetype[MAXLINE], buf[MAXLINE];

    get_filetype(filename, filetype);

    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
    sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
    sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
    Rio_writen(fd, buf, strlen(buf));

    srcfd = Open(filename, O_RDONLY, 0);
    if (filesize < 1024 * 1024) { // 小文件使用直接read
        char *filebuf = (char *)Malloc(filesize);
        Read(srcfd, filebuf, filesize);
        Rio_writen(fd, filebuf, filesize);
        Free(filebuf);
    } else { // 大文件使用mmap
        srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
        Rio_writen(fd, srcp, filesize);
        Munmap(srcp, filesize);
    }
    Close(srcfd);
}

void get_filetype(char *filename, char *filetype) 
{
    if (strstr(filename, ".html"))
        strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif"))
        strcpy(filetype, "image/gif");
    else if (strstr(filename, ".jpg") || strstr(filename, ".jpeg") || strstr(filename, ".jpe"))
        strcpy(filetype, "image/jpeg");
    else if (strstr(filename, ".png"))
        strcpy(filetype, "image/png");
    else if (strstr(filename, ".mpg") || strstr(filename, ".mpeg") || strstr(filename, ".mpe"))
        strcpy(filetype, "video/mpeg");
    else
        strcpy(filetype, "text/plain");
}

void serve_dynamic(int fd, char *filename, char *cgiargs) 
{
    char buf[MAXLINE], *emptylist[] = { NULL };

    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));

    if (Fork() == 0) { 
        setenv("QUERY_STRING", cgiargs, 1); 
        Dup2(fd, STDOUT_FILENO);         
        Execve(filename, emptylist, environ); 
    }
}

void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg) 
{
    char buf[MAXLINE], body[MAXBUF];

    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}