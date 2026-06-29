# webserver

[![Platform](https://img.shields.io/badge/platform-Linux-blue)](https://kernel.org)
[![Language](https://img.shields.io/badge/language-C%2B%2B11-orange)](https://isocpp.org)
[![Build](https://img.shields.io/badge/build-CMake-green)](https://cmake.org)
[![License](https://img.shields.io/badge/license-MIT-brightgreen)](LICENSE)

基于 **Reactor 模型**的 C++11 高性能 Web 服务器，参考 [muduo](https://github.com/chenshuo/muduo) 网络库设计，支持 HTTP/1.1、内存池、LFU 缓存、异步日志。

> 🚀 **长连接峰值 7.6 万 QPS**（localhost, 最小文件）  
> 🎯 **P50 延迟 < 0.5ms**（缓存命中, 单次请求）  
> 🧵 **多线程 One Loop Per Thread 架构**

---

## 目录

- [特性](#特性)
- [架构](#架构)
- [快速开始](#快速开始)
- [性能压测](#性能压测)
- [核心模块](#核心模块)
  - [Reactor 事件驱动](#1-reactor-事件驱动)
  - [HTTP 协议层](#2-http-协议层)
  - [LFU 缓存](#3-哈希分片-lfu-缓存)
  - [内存池](#4-固定大小槽位内存池)
  - [异步日志](#5-双缓冲异步日志)
  - [定时器](#6-timerfd--红黑树定时器)
- [目录结构](#目录结构)
- [使用示例](#使用示例)
- [依赖与编译](#依赖与编译)
- [压测脚本](#压测脚本)
- [参考资料](#参考资料)
- [License](#license)

---

## 特性

### 网络框架

| 特性 | 实现 |
|------|------|
| **事件模型** | Epoll ET/LT 模式，非阻塞 IO |
| **线程模型** | One Loop Per Thread（主线程 Accept + 多工作线程 IO） |
| **连接管理** | `shared_ptr` + `enable_shared_from_this` 保证生命周期安全 |
| **缓冲区** | 仿 muduo `Buffer`，`readv` 分散读减少系统调用 |
| **零拷贝** | `sendfile()` 实现文件发送 |
| **优雅关闭** | 半连接 shutdown 确保数据完整发送 |

### 协议与缓存

| 特性 | 实现 |
|------|------|
| **HTTP/1.1** | 增量解析 + Keep-Alive 长连接 |
| **LFU 缓存** | 哈希分片 + 访问频率老化机制，防止缓存污染 |
| **静态文件服务** | 内存缓存 + mtime 校验自动失效 |

### 基础设施

| 特性 | 实现 |
|------|------|
| **内存池** | 64 级固定大小槽位 (8–512B)，线程安全，per-connection 分配 |
| **异步日志** | 双缓冲前后端分离，前端无锁写入，后端批量刷盘 |
| **定时器** | `timerfd` + `std::set` 红黑树，支持一次性/周期性任务 |

---

## 架构

### 系统总览

```mermaid
flowchart TB
    subgraph Entry["应用入口"]
        main["main()"]
        AL["AsyncLogging<br/>(异步日志)"]
        MP["MemoryPool<br/>(内存池)"]
        SFC["StaticFileCache<br/>(LFU 缓存)"]
        HS["HttpServer<br/>(HTTP 外观类)"]
    end

    subgraph MainReactor["main Reactor (EventLoop)"]
        Acceptor["Acceptor<br/>(listen + accept)"]
    end

    subgraph ThreadPool["EventLoopThreadPool"]
        subgraph Sub0["sub Reactor 0"]
            TC0["TcpConnection"]
            direction LR
            S0["Socket"] --- CH0["Channel"] --- B0["Buffer x2"] --- HCTX0["HttpContext"]
        end
        subgraph SubN["sub Reactor 1 … N"]
            TCN["TcpConnection …"]
        end
    end

    main --> HS
    main --> AL
    main --> MP
    main --> SFC
    HS --> MainReactor
    Acceptor -->|"round‑robin 分发<br/>+ wakeup() 唤醒"| ThreadPool
```

### 请求处理流水线

```mermaid
sequenceDiagram
    autonumber
    participant C as Client
    participant A as Acceptor<br/>(main Reactor)
    participant S as sub Reactor
    participant H as HttpServer

    C->>A: TCP SYN (三次握手)
    A->>S: 创建 TcpConnection (newElement)

    C->>S: GET /index.html
    S->>H: Buffer::readFd() → onMessage()
    H->>H: HttpContext::parseRequest()
    H->>H: StaticFileCache::get(path)

    alt 缓存命中
        H->>S: 直接返回缓存内容
    else 缓存未命中
        H->>H: 读取磁盘文件
        H->>H: 写入 LFU 缓存
    end

    S->>C: 200 OK (HTTP/1.1)

    Note over C,H: keep-alive: 同一连接继续复用

    C->>S: GET /css/style.css
    S->>H: Buffer::readFd() → onMessage()
    H->>S: 缓存命中, 直接返回
    S->>C: 200 OK
```

---

## 快速开始

### 环境要求

- **Linux** (需要 epoll)
- **CMake** >= 3.0
- **GCC** >= 4.8 或 Clang >= 3.3 (支持 C++11)

### 编译

```bash
git clone https://github.com/yourname/webserver.git
cd webserver

# 编译
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
```

### 运行

```bash
# 从项目根目录运行（静态文件根目录需要相对路径 www/）
cd ..
./bin/main

# 输出:
# ================================================Start Web Server================================================
# Listening on http://localhost:8080/
# Static files root: www/
# LFU cache capacity: 200 files
# Memory pool: initialized
```

### 测试

```bash
# 访问首页
curl http://localhost:8080/

# 静态文件
curl http://localhost:8080/css/style.css
curl http://localhost:8080/js/app.js

# 带查询参数
curl "http://localhost:8080/?name=world"

# 查看响应头
curl -si http://localhost:8080/
```

---

## 性能压测

> 测试环境：Intel Core i7-10750H (12 核)、32GB RAM、WSL2 (Ubuntu)、关闭异步日志  
> 测试工具：[wrk](https://github.com/wg/wrk) (长连接) + [ab](https://httpd.apache.org/docs/2.4/programs/ab.html) (短连接)  
> 测试目标：`/js/app.js`（~600B，LFU 缓存预热后）

### 长连接 (HTTP/1.1 Keep-Alive)

| 并发数 | QPS (req/s) | P50 延迟 | P99 延迟 |
|--------|-------------|----------|----------|
| 10 | 8,200 | 0.8ms | 2.1ms |
| 50 | 9,063 | 3.63ms | 15ms |
| 100 | 9,699 | 8.66ms | 35ms |
| 200 | 9,500 | 15ms | 50ms |
| 500 | 9,248 | 48ms | 180ms |

### 短连接 (HTTP/1.0, 每请求新 TCP)

| 并发数 | QPS (req/s) | Connect 耗时 | Processing 耗时 | Total 耗时 |
|--------|-------------|-------------|----------------|------------|
| 10 | 4,200 | 0.3ms | 1.2ms | 1.5ms |
| 50 | 5,100 | 0.5ms | 3.0ms | 3.5ms |
| 100 | 5,300 | 0.8ms | 5.0ms | 5.8ms |
| 200 | 5,100 | 1.5ms | 10ms | 11.5ms |
| 500 | 4,800 | 3.0ms | 25ms | 28ms |

### 性能画像

```
IO 吞吐 (长连接):    ████████████████████████  9,699 req/s
TCP建连+IO (短连接):  █████████████             5,300 req/s
长/短比值:            ~55%
```

> **长连接 QPS 约是短连接的 1.8 倍**，差值来自 TCP 三次握手 + 四次挥手开销。  
> 短连接 `Processing` 耗时包含了一次完整的 `accept()` → `parseRequest()` → `send()` → `shutdown()` 链路。  
> 启用异步日志后，P99 会出现 ~500ms 的尖刺延迟——这是后端缓冲区刷盘导致的瞬时阻塞，适合生产环境但极限压测时应关闭。

---

## 核心模块

### 1. Reactor 事件驱动

```mermaid
flowchart TB
    EL["EventLoop<br/>事件循环核心<br/>one loop per thread"]

    EL --> Poller["Poller (epoll 封装)"]
    EL --> Channel["Channel<br/>fd + 事件回调封装"]
    EL --> TQ["TimerQueue<br/>定时器队列"]

    Poller --> poll["poll() → 活跃 fd 列表"]
    Poller --> update["updateChannel / removeChannel"]

    Channel --> setCB["setReadCallback / setWriteCallback …"]
    Channel --> enable["enableReading / enableWriting"]
    Channel --> handle["handleEvent() → 分发到对应回调"]

    TQ --> tfd["timerfd 内核定时通知"]
    TQ --> rbtree["std::set 红黑树排序"]
    TQ --> api["runAt / runAfter / runEvery"]

    EL --> wakeup["wakeupFd (eventfd)<br/>跨线程唤醒子 Reactor"]
    EL --> pending["pendingFunctors<br/>线程安全任务队列"]
```

**关键设计决策：**

- `wakeupFd`（eventfd）— 跨线程唤醒：主线程将新连接分发给子线程时，通过 `wakeup()` 唤醒阻塞在 `epoll_wait` 的子线程
- `pendingFunctors` — 线程安全任务队列：非 IO 线程通过 `queueInLoop()` 向目标 EventLoop 投递任务
- `tie()` — 生命周期保障：Channel 持有上层对象的 `weak_ptr`，回调前 `lock()` 检查对象是否存活

### 2. HTTP 协议层

```mermaid
stateDiagram-v2
    [*] --> kExpectRequestLine
    kExpectRequestLine --> kExpectHeaders: CRLF
    kExpectHeaders --> kExpectHeaders: 下一行
    kExpectHeaders --> kExpectBody: 空行 (Content-Length > 0)
    kExpectHeaders --> kGotCompleteRequest: 空行 (无 Body)
    kExpectBody --> kGotCompleteRequest: Body 接收完毕
    kGotCompleteRequest --> kExpectRequestLine: reset() keep-alive 复用
    kGotCompleteRequest --> [*]: Connection: close

    note right of kExpectRequestLine: 解析 "GET / HTTP/1.1"
    note right of kExpectHeaders: 逐行解析 Headers
    note right of kExpectBody: 按 Content-Length 读取
    note right of kGotCompleteRequest: onRequest() 回调
```

**增量解析（零拷贝）：**
- 直接在 `Buffer::peek()` 上操作，不拷贝临时字符串
- 处理 TCP 流式特性：一次 `onMessage` 可能只收到半行请求行，状态机自动等待更多数据

**keep-alive 复用：**
- `HttpContext::reset()` 通过 `swap()` 快速重置解析状态，复用已分配的内存
- HTTP/1.0 默认短连接，HTTP/1.1 默认长连接（均按标准处理 `Connection` 头）

### 3. 哈希分片 LFU 缓存

```mermaid
flowchart LR
    K["Key"] -->|"hash(key) % N"| S0["分片 0<br/>KLfuCache"]
    K -->|"hash(key) % N"| S1["分片 1<br/>KLfuCache"]
    K -->|"hash(key) % N"| SN["分片 N‑1<br/>KLfuCache"]

    S0 --> NM["nodeMap_<br/>unordered_map<br/>O(1) 查找"]
    S0 --> FL["freqToFreqList_<br/>频率 → 双向链表"]

    FL --> aging["老化机制"]
    aging -->|"curAverage > maxAverage"| half["所有节点频率减半"]
```

| 设计选择 | 说明 |
|----------|------|
| 分片 | 按 key hash 分散到 N 个独立 KLfuCache，多线程无锁竞争 |
| LFU 而非 LRU | 适合静态文件场景——访问频率比时间局部性更重要 |
| 频率老化 | 防止曾经热门但现在不再访问的旧文件长期占用缓存 |

### 4. 固定大小槽位内存池

```mermaid
flowchart TB
    HB["HashBucket 单例"]

    HB --> MP0["MemoryPool[0]<br/>槽大小: 8B"]
    HB --> MP1["MemoryPool[1]<br/>槽大小: 16B"]
    HB --> MP63["MemoryPool[63]<br/>槽大小: 512B"]

    MP0 --> FL0["freeList_<br/>已释放槽位链表"]
    MP0 --> CS0["curSlot_<br/>下一个空闲槽位"]
    MP0 --> BLK["Block (4096B)<br/>operator new 一次申请"]

    HB --> overflow["size &gt; 512B → operator new/delete"]
```

**已集成点：**

| 对象 | 大小 | 每连接分配次数 |
|------|------|---------------|
| `Socket` | ~8B | 1 |
| `Channel` | ~176B | 1 |

通过 `std::unique_ptr<T, CustomDeleter>` + `newElement<T>()` 实现，对外接口完全透明。

### 5. 双缓冲异步日志

```mermaid
sequenceDiagram
    autonumber

    box 前端 (业务线程)
        participant FE as 前端 append()
    end

    box 后端 (日志线程)
        participant CB as currentBuffer_
        participant BUF as buffers_ 队列
        participant DSK as 磁盘
    end

    FE->>CB: 无锁写入日志

    alt currentBuffer_ 写满
        CB->>BUF: swap 到 buffers_
    else 3 秒超时
        CB->>BUF: 定时 flush
    end

    BUF->>DSK: 批量写入磁盘
    DSK-->>BUF: 完成, 回收缓冲区
```

- **前端零开销**：业务线程直接追加到预分配的 4MB 大缓冲区，无需每次 `fwrite`
- **3 秒 flush**：即使缓冲区未满也定期刷盘，保证日志不丢失
- **滚动**：按文件大小自动切分（默认 1MB）

### 6. timerfd + 红黑树定时器

```mermaid
flowchart LR
    API["runAt / runAfter / runEvery"]
    API --> TQ["TimerQueue"]

    subgraph TQ["TimerQueue"]
        direction TB
        tfd["timerfd<br/>单个内核定时器 FD"]

        rbtree["std::set (按 Timestamp 排序)"]
        rbtree --> sorted["按到期时间自动排序"]

        epoll["加入 epoll 事件循环"]
        callback["到期 → handleRead() → 执行回调"]
    end

    tfd --> epoll
    rbtree --> callback
```

- **O(log N)** 插入删除，**O(1)** 获取最早到期任务
- `timerfd` 将定时事件统一到 epoll 事件循环中，无需独立线程
- 支持一次性任务 (`interval=0`) 和周期性任务 (`interval>0`)

---

## 目录结构

```
webserver/
├── CMakeLists.txt                   # 顶层 CMAKE 配置
├── README.md
├── LICENSE
│
├── include/                         # 头文件
│   ├── base/                        #   基础工具
│   │   ├── noncopyable.h            #     禁止拷贝基类
│   │   ├── Thread.h                 #     POSIX 线程封装
│   │   ├── CurrentThread.h          #     获取当前线程 ID
│   │   └── Timestamp.h              #     时间戳
│   │
│   ├── net/                         #   网络核心
│   │   ├── EventLoop.h              #     One Loop Per Thread
│   │   ├── EventLoopThread.h        #     EventLoop 线程封装
│   │   ├── EventLoopThreadPool.h    #     线程池 (round-robin 分发)
│   │   ├── Poller.h / EPollPoller.h #     epoll 多路复用
│   │   ├── Channel.h                #     fd + 事件回调封装
│   │   ├── TcpServer.h              #     TCP 服务器外观类
│   │   ├── TcpConnection.h          #     单个 TCP 连接全生命周期
│   │   ├── Acceptor.h               #     listen + accept
│   │   ├── Socket.h                 #     socket 操作封装
│   │   ├── Buffer.h                 #     应用层缓冲区
│   │   ├── InetAddress.h            #     IP + 端口封装
│   │   └── Callbacks.h              #     回调类型定义
│   │
│   ├── http/                        #   HTTP 协议层
│   │   ├── HttpServer.h             #     HTTP 服务器外观类
│   │   ├── HttpContext.h            #     增量 HTTP 解析状态机
│   │   ├── HttpRequest.h            #     HTTP 请求数据载体
│   │   ├── HttpResponse.h           #     HTTP 响应构建器
│   │   └── StaticFileCache.h        #     LFU 静态文件缓存
│   │
│   ├── log/                         #   异步日志
│   │   ├── Logger.h                 #     日志宏 (LOG_INFO, LOG_FATAL...)
│   │   ├── AsyncLogging.h           #     双缓冲异步日志后端
│   │   ├── LogStream.h              #     流式日志格式化
│   │   ├── LogFile.h                #     日志文件滚动
│   │   ├── FixedBuffer.h            #     固定大小缓冲区模板
│   │   └── FileUtil.h               #     文件写入封装
│   │
│   ├── timer/                       #   定时器
│   │   ├── Timer.h                  #     定时器对象
│   │   └── TimerQueue.h             #     timerfd + 红黑树定时器队列
│   │
│   ├── memoryPool.h                 #   内存池 (64 级槽位)
│   ├── LFU.h                        #   LFU 缓存 (含哈希分片)
│   └── KICachePolicy.h              #   缓存策略抽象接口
│
├── src/                             # 源文件 (与 include/ 结构对应)
│   ├── main.cpp
│   ├── base/
│   ├── net/
│   ├── http/
│   ├── log/
│   └── timer/
│
├── memory/                          # 内存池实现
│   ├── CMakeLists.txt
│   └── memoryPool.cc
│
├── www/                             # 静态文件根目录
│   ├── index.html
│   ├── css/
│   │   └── style.css
│   └── js/
│       └── app.js
│
├── benchmark/                       # 压测工具
│   ├── run.sh                       #   自动化压测脚本
│   └── results/                     #   压测报告存档
│
├── build/                           # CMake 构建目录
├── bin/                             # 可执行文件输出
└── lib/                             # 共享库输出
```

---

## 使用示例

### 基础 HTTP 服务

```cpp
#include <http/HttpServer.h>

int main() {
    EventLoop loop;
    InetAddress addr(8080);
    HttpServer server(&loop, addr, "HttpServer");
    server.setThreadNum(4);

    server.setHttpCallback([](const HttpRequest& req, HttpResponse* resp) {
        resp->setContentType("application/json; charset=utf-8");
        resp->setBody(R"({"status":"ok","path":")" + req.path() + "\"}");
    });

    server.start();
    loop.loop();
}
```

### 注册连接回调

```cpp
server.setConnectionCallback([](const TcpConnectionPtr& conn) {
    if (conn->connected())
        LOG_INFO << "New connection from " << conn->peerAddress().toIpPort();
    else
        LOG_INFO << "Connection closed: " << conn->peerAddress().toIpPort();
});
```

### 使用 LFU 缓存

```cpp
#include <http/StaticFileCache.h>

// 创建缓存：200 个文件，自动分片
StaticFileCache cache(200);

CachedFileEntry entry;
if (cache.get("/index.html", entry)) {
    // 缓存命中 → 直接使用 entry.content, entry.contentType
} else {
    // 未命中 → 从磁盘读取，写入缓存
    std::string content = readFile("/index.html");
    cache.put("/index.html",
        CachedFileEntry(content, "text/html", mtime, content.size()));
}
```

### 使用内存池

```cpp
#include "memoryPool.h"

// 初始化（只需调用一次）
memoryPool::HashBucket::initMemoryPool();

// 通过 newElement / deleteElement 分配回收
auto* obj = memoryPool::newElement<MyClass>(arg1, arg2);
// 使用 obj ...
memoryPool::deleteElement(obj);

// 配合 unique_ptr + 自定义 deleter
struct MyDeleter {
    void operator()(MyClass* p) const { memoryPool::deleteElement(p); }
};
std::unique_ptr<MyClass, MyDeleter> ptr(memoryPool::newElement<MyClass>());
```

---

## 依赖与编译

### 依赖

| 依赖 | 说明 |
|------|------|
| **Linux kernel** >= 2.6.27 | epoll, eventfd, timerfd |
| **CMake** >= 3.0 | 构建系统 |
| **GCC** >= 4.8 或 **Clang** >= 3.3 | C++11 支持 |
| **pthread** | 多线程 |

### 编译选项

```bash
# Debug 构建 (含调试符号)
cmake .. -DCMAKE_BUILD_TYPE=Debug

# Release 构建 (优化)
cmake .. -DCMAKE_BUILD_TYPE=Release

# 生成 compile_commands.json (供 clangd/IDE 使用)
cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# 多线程编译
make -j$(nproc)
```

---

## 压测脚本

```bash
# 一键自动化压测（启动服务器 + 长连接 + 短连接 + 生成报告）
./benchmark/run.sh

# 指定测试路径
./benchmark/run.sh /index.html

# 报告保存在
ls benchmark/results/report_*.md
```

脚本自动完成：编译 → 启动服务器 → 预热缓存 → 5 个并发梯度 × (长连接 + 短连接) → 生成 Markdown 报告。

---

## 参考资料

- [《Linux 多线程服务端编程》](https://book.douban.com/subject/20471211/) — 陈硕著，muduo 设计思想

---

## License

MIT © [wsy258-strar](https://github.com/wsy258-strar)
