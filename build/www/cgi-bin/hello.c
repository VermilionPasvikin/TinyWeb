#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

int main(void)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timestr[128];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", tm);

    char *qs = getenv("QUERY_STRING");
    if (!qs) qs = "";

    printf("Content-Type: text/html\r\n\r\n");
    printf("<!doctype html>\n");
    printf("<html lang=\"zh-CN\">\n");
    printf("<head><meta charset=\"utf-8\"><title>CGI 测试页面</title></head>\n");
    printf("<body>\n");
    printf("<h1>CGI 测试程序</h1>\n");
    printf("<p>当前服务器时间：%s</p>\n", timestr);
    printf("<p>传入的 QUERY_STRING：%s</p>\n", qs);
    printf("<p>随机数示例：%d</p>\n", rand() % 1000);
    printf("<hr>\n");
    printf("<p>此页面由 %s 动态生成（无超链接）。</p>\n", "hello.c");
    printf("</body>\n</html>\n");

    return 0;
}