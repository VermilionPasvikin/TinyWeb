#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timestr[128];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", tm);

    char *method = getenv("REQUEST_METHOD");
    char *query = getenv("QUERY_STRING");
    char *content_length = getenv("CONTENT_LENGTH");
    char *content_type = getenv("CONTENT_TYPE");

    if (!method) method = "未设置";
    if (!query) query = "";
    if (!content_length) content_length = "0";
    if (!content_type) content_type = "未设置";

    printf("Content-Type: text/html\r\n\r\n");
    printf("<!doctype html>\n");
    printf("<html lang=\"zh-CN\">\n");
    printf("<head><meta charset=\"utf-8\"><title>CGI 环境信息</title></head>\n");
    printf("<body style=\"font-family: sans-serif; padding: 2rem; line-height: 1.6;\">\n");
    printf("<h1>CGI 环境信息</h1>\n");

    printf("<h2>服务器信息</h2>\n");
    printf("<table border=\"1\" cellpadding=\"10\" style=\"border-collapse: collapse;\">\n");
    printf("<tr><th style=\"text-align: left;\">项目</th><th style=\"text-align: left;\">值</th></tr>\n");
    printf("<tr><td>当前时间</td><td>%s</td></tr>\n", timestr);
    printf("<tr><td>进程 ID</td><td>%d</td></tr>\n", getpid());
    printf("</table>\n");

    printf("<h2>CGI 环境变量</h2>\n");
    printf("<table border=\"1\" cellpadding=\"10\" style=\"border-collapse: collapse;\">\n");
    printf("<tr><th style=\"text-align: left;\">变量名</th><th style=\"text-align: left;\">值</th></tr>\n");
    printf("<tr><td>REQUEST_METHOD</td><td><strong>%s</strong></td></tr>\n", method);
    printf("<tr><td>QUERY_STRING</td><td>%s</td></tr>\n", query);
    printf("<tr><td>CONTENT_LENGTH</td><td>%s</td></tr>\n", content_length);
    printf("<tr><td>CONTENT_TYPE</td><td>%s</td></tr>\n", content_type);
    printf("</table>\n");

    printf("<hr>\n");
    printf("<p><a href=\"/cgi-demo.html\">返回 CGI 演示页面</a></p>\n");
    printf("</body>\n</html>\n");

    return 0;
}
