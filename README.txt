TinyWeb
=======

一个用 C 语言编写的轻量级多线程 HTTP 服务器，附带专属反向代理 TinyProxy。
基于 CSAPP（深入理解计算机系统）的 tiny web server 框架扩展而来。


项目结构
--------

  src/          源代码
    main.c        TinyWeb 主程序（线程池、请求处理、CGI）
    proxy.c       TinyProxy 主程序（反向代理、负载均衡、健康检查）
    admin.c       后台管理页面与 JSON API
    csapp.c/h     底层封装库（I/O、信号、线程、网络）
    sbuf.c/h      线程安全生产者-消费者队列
    sio.c/h       异步信号安全 I/O
    proxy_config.h  后端配置结构定义

  www/          站点根目录（构建时自动复制到 build/ 旁边）
    index.html    示例页面
    cgi-bin/      CGI 程序目录（.c 文件在构建时自动编译）

  build/        构建输出目录


构建
----

依赖：CMake >= 3.10，C11 编译器（clang 或 gcc）

  mkdir -p build && cd build
  cmake ..
  make

构建产物：
  build/TinyWeb       Web 服务器可执行文件
  build/TinyProxy     反向代理可执行文件
  build/www/          站点文件（含编译好的 CGI 程序）


TinyWeb 使用
------------

  ./TinyWeb <端口> [--admin-port <管理端口>] [--root <站点目录>]

  # 示例：在 8080 端口启动，使用默认 www/ 目录
  ./TinyWeb 8080

  # 启用管理面板（仅本机可访问）
  ./TinyWeb 8080 --admin-port 9090

  # 指定自定义站点目录
  ./TinyWeb 8080 --admin-port 9090 --root /var/www/html

启动后访问：
  http://localhost:8080/               静态文件
  http://localhost:8080/cgi-bin/hello  CGI 动态内容
  http://localhost:9090/__admin/       后台管理面板（需传 --admin-port）

支持的 HTTP 方法：GET、POST、HEAD
支持协议：HTTP/1.0、HTTP/1.1（响应版本跟随请求版本，Connection: close）


TinyProxy 使用
--------------

  ./TinyProxy <监听端口> [--admin-port <管理端口>] <后端host:port> [后端host:port ...]

  # 示例：在 9090 端口监听，轮询转发到两个后端
  ./TinyProxy 9090 localhost:8080 localhost:8081

  # 启用代理管理面板
  ./TinyProxy 9090 --admin-port 9091 localhost:8080 localhost:8081

  # --admin-port 可放在后端列表之前或之后
  ./TinyProxy 9090 localhost:8080 --admin-port 9091

特性：
  - 轮询（Round-Robin）负载均衡，最多 8 个后端
  - 每 3 秒一次的后端健康检查，故障自动摘除、恢复自动加回
  - 自动注入 X-Forwarded-For 头
  - 双向 TCP 中继，select + 30 秒超时


管理面板安全说明
----------------

管理面板通过独立端口提供，仅绑定 127.0.0.1（本机回环地址），
无法从网络其他主机访问。不传 --admin-port 则管理接口完全不启动。

TinyWeb 管理面板（--admin-port 指定的端口）：

  GET  /__admin/          管理页面（实时图表，2 秒刷新）
  GET  /__admin/status    JSON：连接数、总请求数、错误数、运行时长
  GET  /__admin/metrics   JSON：发送字节数、近 60 秒请求速率
  POST /__admin/stop      优雅停止服务器

TinyProxy 管理面板（--admin-port 指定的端口）：

  GET  /__admin/          管理页面（含后端状态表格，2 秒刷新）
  GET  /__admin/status    JSON：连接数、总请求数、错误数、运行时长
  GET  /__admin/metrics   JSON：转发字节数、近 60 秒请求速率
  GET  /__admin/backends  JSON：各后端 host:port、存活状态、请求计数
  POST /__admin/stop      优雅停止代理

注意：TinyProxy 管理面板展示的是代理层自身的统计（经过代理转发的流量），
与各后端实例的 /__admin 数据相互独立。多后端场景下各实例的统计需分别查看。


CGI 开发
--------

将 .c 文件放入 www/cgi-bin/，CMake 构建时自动编译。
CGI 程序通过以下环境变量接收请求信息：

  REQUEST_METHOD    GET / POST
  QUERY_STRING      URL 查询字符串（GET 参数）
  CONTENT_LENGTH    请求体长度（POST 时有效）
  CONTENT_TYPE      请求体类型（POST 时有效）

标准输出（stdout）直接写入 socket，输出格式遵循 HTTP 响应规范：
服务器已预先发送状态行和 Server 头，CGI 程序从响应头的剩余部分开始输出。


并发模型
--------

TinyWeb 和 TinyProxy 均使用相同的线程池架构：
  - 8 个工作线程 + 容量为 32 的连接队列（生产者-消费者模型）
  - CGI 请求通过 fork + execve 执行，子进程由 SIGCHLD 处理器异步回收
  - 平台适配：macOS 使用命名信号量，Linux 使用未命名信号量


注意事项
--------

  - 本项目为学习/实验性质，不建议直接用于生产环境
  - 不支持 HTTPS
  - HTTP/1.1 持久连接未实现（每次请求后关闭连接）
  - CGI 进程无超时限制
