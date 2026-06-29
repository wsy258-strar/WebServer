# webserver

基于 Reactor 模型的 C++11 高性能 Web 服务器，参考 muduo 网络库设计。

## 特性

- **Reactor 事件驱动** — one loop per thread 模型，主线程 accept + 工作线程 IO
- **epoll 多路复用** — LT 模式，支持高并发连接
- **HTTP/1.1** — 增量解析、Keep-Alive、Query 参数解析
- **异步日志** — 双缓冲前后端分离，前端无锁写入，后端批量刷盘
- **LFU 缓存** — 带访问频率老化机制，防止缓存污染
- **内存池** — 自定义内存池，减少频繁 new/delete 开销
- **定时器** — timerfd + 红黑树实现，支持一次性/周期性任务

## 架构

```
main Reactor (EventLoop)
  ├── Acceptor (监听新连接)
  └── EventLoopThreadPool
        ├── sub Reactor 0 (EventLoop)
        │     ├── TcpConnection → HttpContext → HTTP 解析
        │     └── TcpConnection → ...
        ├── sub Reactor 1
        └── sub Reactor N
```

### 核心模块

| 模块 | 说明 |
|------|------|
| `EventLoop` / `Poller` / `Channel` | Reactor 事件驱动核心 |
| `TcpServer` / `TcpConnection` | TCP 网络层 |
| `HttpServer` / `HttpContext` | HTTP 协议层 |
| `AsyncLogging` / `Logger` | 异步日志系统 |
| `LFU` / `memoryPool` | 缓存与内存池 |

## 快速开始

### 编译

```bash
cd build && cmake .. && make -j$(nproc)
```

### 运行

```bash
./bin/main
# 输出: Listening on http://localhost:8080/
```

### 测试

```bash
# 基本请求
curl http://localhost:8080/

# 带参数请求
curl "http://localhost:8080/search?q=hello&page=1"

# 检查响应头
curl -s -v http://localhost:8080/ 2>&1 | grep "^< "
```

## 依赖

- Linux (epoll)
- CMake >= 3.0
- GCC >= 4.8 (支持 C++11)

## 目录结构

```
webserver/
├── CMakeLists.txt
├── include/
│   ├── base/     # 基础工具 (Timestamp, Thread, noncopyable)
│   ├── net/      # 网络核心 (EventLoop, TcpServer, Buffer...)
│   ├── http/     # HTTP 层 (HttpServer, HttpContext...)
│   ├── log/      # 日志系统 (Logger, AsyncLogging...)
│   └── timer/    # 定时器
├── src/
│   ├── base/  net/  http/  log/  timer/
│   └── main.cpp
└── memory/       # 内存池 + LFU 缓存
```

## License

MIT
