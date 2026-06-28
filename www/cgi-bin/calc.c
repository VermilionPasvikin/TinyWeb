#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

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

    char decoded[256];
    url_decode(decoded, value);
    strncpy(value, decoded, max_len - 1);
    value[max_len - 1] = '\0';

    return 1;
}

int main(void)
{
    char *content_length_str = getenv("CONTENT_LENGTH");
    int content_length = 0;

    if (content_length_str) {
        content_length = atoi(content_length_str);
    }

    if (content_length <= 0 || content_length > MAX_INPUT) {
        char error[] = "Content-Type: application/json\r\n\r\n"
                       "{\"status\": \"error\", \"message\": \"Invalid content length\"}\n";
        write(STDOUT_FILENO, error, strlen(error));
        return 1;
    }

    char buffer[MAX_INPUT];
    // 使用read()直接从stdin读取
    int bytes_read = read(STDIN_FILENO, buffer, content_length);
    if (bytes_read < 0) bytes_read = 0;
    buffer[bytes_read] = '\0';

    char num1_str[64], num2_str[64], operation[64];
    if (!get_param(buffer, "num1", num1_str, sizeof(num1_str)) ||
        !get_param(buffer, "num2", num2_str, sizeof(num2_str)) ||
        !get_param(buffer, "operation", operation, sizeof(operation))) {
        char error[] = "Content-Type: application/json\r\n\r\n"
                       "{\"status\": \"error\", \"message\": \"Missing parameters\"}\n";
        write(STDOUT_FILENO, error, strlen(error));
        return 1;
    }

    double num1 = atof(num1_str);
    double num2 = atof(num2_str);
    double result = 0;
    int valid = 1;
    char error_msg[256] = "";

    if (strcmp(operation, "add") == 0) {
        result = num1 + num2;
    } else if (strcmp(operation, "subtract") == 0) {
        result = num1 - num2;
    } else if (strcmp(operation, "multiply") == 0) {
        result = num1 * num2;
    } else if (strcmp(operation, "divide") == 0) {
        if (num2 == 0) {
            valid = 0;
            snprintf(error_msg, sizeof(error_msg), "除数不能为零");
        } else {
            result = num1 / num2;
        }
    } else {
        valid = 0;
        snprintf(error_msg, sizeof(error_msg), "未知的运算: %s", operation);
    }

    // 使用write()直接输出
    char response[1024];
    int len = snprintf(response, sizeof(response), "Content-Type: application/json\r\n\r\n");

    if (valid) {
        len += snprintf(response + len, sizeof(response) - len,
                       "{\"status\": \"success\", \"result\": %.6g, \"operation\": \"%s\"}\n",
                       result, operation);
    } else {
        len += snprintf(response + len, sizeof(response) - len,
                       "{\"status\": \"error\", \"message\": \"%s\"}\n", error_msg);
    }

    write(STDOUT_FILENO, response, len);

    return 0;
}
