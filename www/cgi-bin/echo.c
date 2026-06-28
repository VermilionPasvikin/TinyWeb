#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_INPUT 4096

int main(void)
{
    char *method = getenv("REQUEST_METHOD");
    char *query = getenv("QUERY_STRING");
    char *content_length_str = getenv("CONTENT_LENGTH");
    char *content_type = getenv("CONTENT_TYPE");

    if (!method) method = "GET";
    if (!query) query = "";
    if (!content_type) content_type = "application/octet-stream";

    int content_length = 0;
    if (content_length_str) {
        content_length = atoi(content_length_str);
    }

    // 使用write()直接写入响应头
    char header[512];
    int hlen = snprintf(header, sizeof(header), "Content-Type: application/json\r\n\r\n");
    write(STDOUT_FILENO, header, hlen);

    // 开始JSON
    char response[MAX_INPUT * 2];
    int len = snprintf(response, sizeof(response),
        "{\n  \"method\": \"%s\",\n  \"query_string\": \"%s\",\n  \"content_type\": \"%s\",\n",
        method, query, content_type);

    if (strcmp(method, "POST") == 0 && content_length > 0) {
        char buffer[MAX_INPUT];
        int bytes_to_read = content_length < MAX_INPUT - 1 ? content_length : MAX_INPUT - 1;

        // 使用read()直接从stdin读取
        int bytes_read = read(STDIN_FILENO, buffer, bytes_to_read);
        if (bytes_read < 0) bytes_read = 0;
        buffer[bytes_read] = '\0';

        len += snprintf(response + len, sizeof(response) - len,
            "  \"content_length\": %d,\n  \"post_data\": \"", content_length);

        // 转义特殊字符
        for (int i = 0; i < bytes_read && len < sizeof(response) - 10; i++) {
            if (buffer[i] == '"' || buffer[i] == '\\') {
                response[len++] = '\\';
            }
            if (buffer[i] >= 32 && buffer[i] < 127) {
                response[len++] = buffer[i];
            } else {
                len += snprintf(response + len, sizeof(response) - len,
                    "\\x%02x", (unsigned char)buffer[i]);
            }
        }
        len += snprintf(response + len, sizeof(response) - len, "\"\n");
    } else {
        len += snprintf(response + len, sizeof(response) - len,
            "  \"content_length\": 0,\n  \"post_data\": null\n");
    }

    len += snprintf(response + len, sizeof(response) - len, "}\n");

    // 使用write()直接输出
    write(STDOUT_FILENO, response, len);

    return 0;
}
