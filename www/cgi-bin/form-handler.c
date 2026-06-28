#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#define MAX_INPUT 4096

void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if (*src == '%' && ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= 'A' - 10;
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= 'A' - 10;
            else b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

int get_param(const char *data, const char *param, char *value, int max_len) {
    char search[256];
    snprintf(search, sizeof(search), "%s=", param);

    const char *start = strstr(data, search);
    if (!start) {
        value[0] = '\0';
        return 0;
    }

    start += strlen(search);
    const char *end = strchr(start, '&');
    int len = end ? (end - start) : strlen(start);

    if (len >= max_len) len = max_len - 1;
    strncpy(value, start, len);
    value[len] = '\0';

    char decoded[512];
    url_decode(decoded, value);
    strncpy(value, decoded, max_len - 1);
    value[max_len - 1] = '\0';

    return 1;
}

void html_escape(const char *src, char *dst, int max_len) {
    int j = 0;
    for (int i = 0; src[i] && j < max_len - 10; i++) {
        if (src[i] == '<') {
            strcpy(&dst[j], "&lt;");
            j += 4;
        } else if (src[i] == '>') {
            strcpy(&dst[j], "&gt;");
            j += 4;
        } else if (src[i] == '&') {
            strcpy(&dst[j], "&amp;");
            j += 5;
        } else if (src[i] == '"') {
            strcpy(&dst[j], "&quot;");
            j += 6;
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

int main(void)
{
    char *content_length_str = getenv("CONTENT_LENGTH");
    int content_length = 0;

    if (content_length_str) {
        content_length = atoi(content_length_str);
    }

    if (content_length <= 0 || content_length > MAX_INPUT) {
        printf("Content-Type: text/html\r\n\r\n");
        printf("<h2>错误</h2><p>无效的请求数据</p>\n");
        return 1;
    }

    char buffer[MAX_INPUT];
    int bytes_read = fread(buffer, 1, content_length, stdin);
    buffer[bytes_read] = '\0';

    char name[256], email[256], message[1024];
    get_param(buffer, "name", name, sizeof(name));
    get_param(buffer, "email", email, sizeof(email));
    get_param(buffer, "message", message, sizeof(message));

    if (strlen(name) == 0 || strlen(email) == 0 || strlen(message) == 0) {
        printf("Content-Type: text/html\r\n\r\n");
        printf("<h2>提交失败</h2><p>所有字段都是必填的</p>\n");
        return 1;
    }

    time_t now = time(NULL);
    long submission_id = (long)now;

    char safe_name[512], safe_email[512], safe_message[2048];
    html_escape(name, safe_name, sizeof(safe_name));
    html_escape(email, safe_email, sizeof(safe_email));
    html_escape(message, safe_message, sizeof(safe_message));

    printf("Content-Type: text/html\r\n\r\n");
    printf("<!doctype html>\n");
    printf("<html lang=\"zh-CN\">\n");
    printf("<head><meta charset=\"utf-8\"><title>提交成功</title>");
    printf("<style>body{font-family:sans-serif;padding:2rem;line-height:1.6;max-width:800px;margin:0 auto;}");
    printf(".success{background:#f0fff4;border:2px solid #28a745;padding:1rem;border-radius:6px;margin:1rem 0;}");
    printf("table{width:100%%;border-collapse:collapse;margin:1rem 0;}");
    printf("th,td{padding:0.75rem;text-align:left;border-bottom:1px solid #ddd;}");
    printf("th{background:#f6f8fa;}</style>");
    printf("</head>\n");
    printf("<body>\n");

    printf("<div class=\"success\">\n");
    printf("<h1>✓ 提交成功</h1>\n");
    printf("<p>您的反馈已经成功提交，感谢您的参与！</p>\n");
    printf("</div>\n");

    printf("<h2>提交信息</h2>\n");
    printf("<table>\n");
    printf("<tr><th>项目</th><th>内容</th></tr>\n");
    printf("<tr><td>提交ID</td><td><strong>%ld</strong></td></tr>\n", submission_id);
    printf("<tr><td>姓名</td><td>%s</td></tr>\n", safe_name);
    printf("<tr><td>邮箱</td><td>%s</td></tr>\n", safe_email);
    printf("<tr><td>反馈内容</td><td>%s</td></tr>\n", safe_message);
    printf("</table>\n");

    printf("<p><a href=\"/post-demo.html\">返回表单页面</a> | <a href=\"/\">返回首页</a></p>\n");
    printf("</body>\n</html>\n");

    return 0;
}
