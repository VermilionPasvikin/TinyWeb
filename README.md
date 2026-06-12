# TinyWeb 🌐

> 用八条线程撑起半个互联网（误）

---

## 这是什么？

一个用 C 语言写的 Web 服务器。功能齐全，代码精简，经得起"能跑就行"的工程哲学考验。

它能：
- 📄 **托管静态文件**（HTML、图片、JS……只要是文件它就给你发）
- ⚙️ **执行 CGI 程序**（放进 `cgi-bin/`，fork 一下，人生哲理立马输出到浏览器）
- 🧵 **8 条工作线程**同时处理请求（多了作者觉得没必要，少了你的请求会排队）
- 🔒 **防目录穿越**（`../../../etc/passwd` 这条路在这里是死路）

它不能：
- POST（我们只接受 GET，其它方法请递交申诉函）
- HTTP/1.1（keep-alive？这里没有长情）
- HTTPS（安全是别人的事）

---

## 快速开始

### 编译

```bash
mkdir build && cd build
cmake .. && make
```

### 运行

```bash
# 默认从 www/ 目录托管站点，监听 8080 端口
./TinyWeb 8080

# 自定义站点根目录
./TinyWeb 8080 --root /path/to/your/site
```

然后打开浏览器访问 `http://localhost:8080/`，大功告成。

---

## 项目结构

```
TinyWeb/
├── src/                  # 所有 C 源码
│   ├── main.c            # 服务器主程序（精华所在）
│   ├── csapp.c / csapp.h # CS:APP 包装库（来自教材，经历过无数学生的眼泪）
│   ├── sbuf.c / sbuf.h   # 线程安全的生产者-消费者队列
│   └── sio.c / sio.h     # 信号安全 I/O（在信号处理里 printf 是危险行为）
├── www/                  # 站点根目录（把你的网站文件放这里）
│   ├── index.html        # 首页
│   └── cgi-bin/          # CGI 可执行程序放这里
│       └── hello.c       # CGI 示例（编译后放入同目录）
└── CMakeLists.txt        # 构建配置
```

---

## CGI 使用说明

CGI 程序放进 `www/cgi-bin/`，编译成可执行文件（**不是 `.c` 源文件**），访问路径为 `/cgi-bin/程序名`。

```bash
# 编译 CGI 示例
gcc www/cgi-bin/hello.c -o www/cgi-bin/hello
# 访问
curl http://localhost:8080/cgi-bin/hello
```

CGI 程序通过环境变量 `QUERY_STRING` 接收 URL 参数：
```
/cgi-bin/hello?name=world
```

---

## 命令行参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `<port>` | 监听端口（必填） | — |
| `--root <dir>` | 站点根目录 | `www` |

---

## 技术细节（给好奇的你）

- **并发模型**：线程池 + 生产者-消费者队列（`sbuf_t`），主线程接受连接扔进队列，8 条工作线程抢着处理
- **静态文件**：小于 1MB 的文件直接 `read()`，大文件用 `mmap()` 减少拷贝
- **CGI 执行**：`fork()` + `execve()`，子进程的 stdout 接到客户端 fd，亲切自然
- **跨平台信号量**：macOS 不支持匿名信号量，于是用命名信号量绕过去了（`/tinyweb_terminal_mutex`）
- **信号处理**：`SIGCHLD` 回收 CGI 子进程，`SIGPIPE` 静默忽略，`SIGTERM`/`SIGINT`/`SIGQUIT` 优雅退出

---

## 已知局限（不是 Bug，是特性）

1. **只支持 GET 方法**。想 POST？自己 fork 然后加。
2. **HTTP/1.0**。每次请求都新建连接，vintage 风格。
3. **线程数硬编码为 8**。改 `THREAD_COUNT` 宏然后重编译，不难。
4. **不支持 Windows**。`fork()` 在那边不存在。

---

## 平台支持

| 平台 | 支持状态 |
|------|---------|
| Linux | ✅ 完全支持 |
| macOS | ✅ 完全支持（绕过了几个坑） |
| Windows | ❌ 暂不支持（`fork()` 表示拒绝） |

---

## 许可

见 `LICENSE` 文件。总之，随便用，出了事别找我。
