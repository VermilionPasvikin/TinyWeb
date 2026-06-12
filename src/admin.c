#include "admin.h"
#include "csapp.h"
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>

server_stats_t g_stats = {0};

/* 内嵌管理页面 HTML */
static const char ADMIN_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"zh-CN\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"<title>TinyWeb 管理面板</title>\n"
"<style>\n"
"  * { box-sizing: border-box; margin: 0; padding: 0; }\n"
"  body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;\n"
"         background: #0f172a; color: #e2e8f0; min-height: 100vh; padding: 24px; }\n"
"  h1 { font-size: 1.5rem; font-weight: 700; margin-bottom: 24px;\n"
"       display: flex; align-items: center; gap: 10px; }\n"
"  .dot { width: 12px; height: 12px; border-radius: 50%; background: #22c55e;\n"
"         box-shadow: 0 0 8px #22c55e; animation: pulse 2s infinite; }\n"
"  .dot.dead { background: #ef4444; box-shadow: 0 0 8px #ef4444; animation: none; }\n"
"  @keyframes pulse { 0%,100%{opacity:1} 50%{opacity:0.5} }\n"
"  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px,1fr));\n"
"          gap: 16px; margin-bottom: 24px; }\n"
"  .card { background: #1e293b; border-radius: 12px; padding: 20px;\n"
"          border: 1px solid #334155; }\n"
"  .card-label { font-size: 0.75rem; color: #94a3b8; text-transform: uppercase;\n"
"                letter-spacing: 0.05em; margin-bottom: 8px; }\n"
"  .card-value { font-size: 2rem; font-weight: 700; color: #f1f5f9; }\n"
"  .card-unit { font-size: 0.875rem; color: #64748b; margin-top: 4px; }\n"
"  .chart-wrap { background: #1e293b; border-radius: 12px; padding: 20px;\n"
"                border: 1px solid #334155; margin-bottom: 24px; }\n"
"  .chart-title { font-size: 0.875rem; color: #94a3b8; margin-bottom: 12px; }\n"
"  canvas { width: 100% !important; height: 120px !important; }\n"
"  .actions { display: flex; gap: 12px; }\n"
"  button { padding: 10px 20px; border: none; border-radius: 8px; cursor: pointer;\n"
"           font-size: 0.875rem; font-weight: 600; transition: opacity 0.2s; }\n"
"  button:hover { opacity: 0.8; }\n"
"  .btn-stop { background: #ef4444; color: white; }\n"
"  .btn-refresh { background: #3b82f6; color: white; }\n"
"  .footer { margin-top: 24px; font-size: 0.75rem; color: #475569; }\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<h1><span id=\"dot\" class=\"dot\"></span>TinyWeb 管理面板</h1>\n"
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
"  <div class=\"card\"><div class=\"card-label\">已发送流量</div>\n"
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
"<div class=\"actions\">\n"
"  <button class=\"btn-refresh\" onclick=\"poll()\">立即刷新</button>\n"
"  <button class=\"btn-stop\" onclick=\"stopServer()\">停止服务器</button>\n"
"</div>\n"
"<div class=\"footer\" id=\"last-update\">等待数据...</div>\n"
"<script>\n"
"const canvas = document.getElementById('chart');\n"
"const ctx = canvas.getContext('2d');\n"
"let rpsHistory = new Array(60).fill(0);\n"
"\n"
"function fmtBytes(b) {\n"
"  if (b < 1024) return [b.toFixed(0), 'bytes'];\n"
"  if (b < 1048576) return [(b/1024).toFixed(1), 'KB'];\n"
"  if (b < 1073741824) return [(b/1048576).toFixed(1), 'MB'];\n"
"  return [(b/1073741824).toFixed(2), 'GB'];\n"
"}\n"
"\n"
"function drawChart(data) {\n"
"  const w = canvas.parentElement.clientWidth - 40;\n"
"  const h = 120;\n"
"  canvas.width = w; canvas.height = h;\n"
"  const max = Math.max(...data, 1);\n"
"  ctx.clearRect(0, 0, w, h);\n"
"  ctx.fillStyle = '#0f172a';\n"
"  ctx.fillRect(0, 0, w, h);\n"
"  const step = w / (data.length - 1);\n"
"  ctx.beginPath();\n"
"  ctx.strokeStyle = '#3b82f6';\n"
"  ctx.lineWidth = 2;\n"
"  data.forEach((v, i) => {\n"
"    const x = i * step;\n"
"    const y = h - (v / max) * (h - 10) - 5;\n"
"    i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);\n"
"  });\n"
"  ctx.stroke();\n"
"  ctx.lineTo(w, h); ctx.lineTo(0, h);\n"
"  ctx.fillStyle = 'rgba(59,130,246,0.15)';\n"
"  ctx.fill();\n"
"}\n"
"\n"
"async function poll() {\n"
"  try {\n"
"    const [s, m] = await Promise.all([\n"
"      fetch('/__admin/status').then(r=>r.json()),\n"
"      fetch('/__admin/metrics').then(r=>r.json())\n"
"    ]);\n"
"    document.getElementById('active').textContent = s.active_connections;\n"
"    document.getElementById('total').textContent = s.total_requests;\n"
"    document.getElementById('errors').textContent = s.total_errors;\n"
"    document.getElementById('uptime').textContent = s.uptime_seconds;\n"
"    const [bv, bu] = fmtBytes(m.bytes_sent);\n"
"    document.getElementById('bytes').textContent = bv;\n"
"    document.getElementById('bytes-unit').textContent = bu;\n"
"    const rpsArr = m.req_per_sec;\n"
"    const curRps = rpsArr ? rpsArr[rpsArr.length-1] || 0 : 0;\n"
"    document.getElementById('rps').textContent = curRps;\n"
"    if (rpsArr) { rpsHistory = rpsArr; drawChart(rpsHistory); }\n"
"    document.getElementById('last-update').textContent =\n"
"      '最后更新: ' + new Date().toLocaleTimeString();\n"
"  } catch(e) {\n"
"    document.getElementById('dot').className = 'dot dead';\n"
"    document.getElementById('last-update').textContent = '连接失败: ' + e.message;\n"
"  }\n"
"}\n"
"\n"
"async function stopServer() {\n"
"  if (!confirm('确认停止服务器？')) return;\n"
"  await fetch('/__admin/stop', {method:'POST'});\n"
"  document.getElementById('dot').className = 'dot dead';\n"
"  document.getElementById('last-update').textContent = '服务器已停止';\n"
"}\n"
"\n"
"poll();\n"
"setInterval(poll, 2000);\n"
"</script>\n"
"</body>\n"
"</html>\n";

/* 更新滑动窗口：由 doit() 在每次请求结束时调用 */
void stats_update_window(void) {
    time_t now = time(NULL);
    time_t last = g_stats.last_window_time;
    if (last == 0) {
        g_stats.last_window_time = now;
        return;
    }
    long diff = (long)(now - last);
    if (diff <= 0) {
        /* 同一秒内，直接累加到当前槽 */
        int idx = atomic_load(&g_stats.window_idx);
        atomic_fetch_add(&g_stats.req_per_sec[idx], 1);
        return;
    }
    /* 经过了 diff 秒，将中间跳过的槽清零，推进索引 */
    if (diff > RATE_WINDOW) diff = RATE_WINDOW;
    int cur_idx = atomic_load(&g_stats.window_idx);
    for (long i = 0; i < diff; i++) {
        cur_idx = (cur_idx + 1) % RATE_WINDOW;
        atomic_store(&g_stats.req_per_sec[cur_idx], 0);
    }
    atomic_store(&g_stats.req_per_sec[cur_idx], 1);
    atomic_store(&g_stats.window_idx, cur_idx);
    g_stats.last_window_time = now;
}

/* 发送 HTTP 响应头 */
static void send_headers(int fd, const char *status, const char *content_type, long content_len) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "HTTP/1.0 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, content_type, content_len);
    Rio_writen(fd, buf, strlen(buf));
}

/* GET /__admin/ — 返回管理页面 */
static void handle_index(int fd) {
    long len = (long)strlen(ADMIN_HTML);
    send_headers(fd, "200 OK", "text/html; charset=utf-8", len);
    Rio_writen(fd, (void *)ADMIN_HTML, len);
}

/* GET /__admin/status */
static void handle_status(int fd) {
    time_t now = time(NULL);
    long uptime = (long)(now - g_stats.start_time);
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{"
        "\"uptime_seconds\":%ld,"
        "\"active_connections\":%d,"
        "\"total_requests\":%ld,"
        "\"total_errors\":%ld"
        "}",
        uptime,
        atomic_load(&g_stats.active_connections),
        atomic_load(&g_stats.total_requests),
        atomic_load(&g_stats.total_errors));
    send_headers(fd, "200 OK", "application/json", n);
    Rio_writen(fd, buf, n);
}

/* GET /__admin/metrics */
static void handle_metrics(int fd) {
    char buf[2048];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{\"bytes_sent\":%ld,\"req_per_sec\":[",
        atomic_load(&g_stats.bytes_sent));

    int cur = atomic_load(&g_stats.window_idx);
    /* 从最旧到最新输出，即从 (cur+1) 开始循环 60 个 */
    for (int i = 0; i < RATE_WINDOW; i++) {
        int idx = (cur + 1 + i) % RATE_WINDOW;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%ld%s",
            atomic_load(&g_stats.req_per_sec[idx]),
            (i < RATE_WINDOW - 1) ? "," : "");
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");

    send_headers(fd, "200 OK", "application/json", pos);
    Rio_writen(fd, buf, pos);
}

/* POST /__admin/stop */
static void handle_stop(int fd) {
    const char *body = "{\"status\":\"stopping\"}";
    send_headers(fd, "200 OK", "application/json", (long)strlen(body));
    Rio_writen(fd, (void *)body, strlen(body));
    atomic_fetch_sub(&g_stats.active_connections, 1);
    raise(SIGTERM);
}

/* 主入口：由 doit() 在检测到 /__admin 前缀后调用 */
void admin_handle(rio_t *rio_p, int fd, const char *method, const char *uri) {
    /* 跳过请求头（admin 请求无需解析） */

    char buf[MAXLINE];
    /* 请求行已在 doit() 里读过，这里读剩余头部直到空行 */
    do {
        Rio_readlineb(rio_p, buf, MAXLINE);
    } while (strcmp(buf, "\r\n") != 0 && strlen(buf) > 0);

    if (strcmp(uri, "/__admin/") == 0 || strcmp(uri, "/__admin") == 0) {
        handle_index(fd);
    } else if (strcmp(uri, "/__admin/status") == 0) {
        handle_status(fd);
    } else if (strcmp(uri, "/__admin/metrics") == 0) {
        handle_metrics(fd);
    } else if (strcmp(uri, "/__admin/stop") == 0 &&
               strcasecmp(method, "POST") == 0) {
        handle_stop(fd);
    } else {
        const char *body = "404 Not Found";
        send_headers(fd, "404 Not Found", "text/plain", (long)strlen(body));
        Rio_writen(fd, (void *)body, strlen(body));
    }
}