# kama-webserver
【代码随想录知识星球】项目分享-webserver

## 项目介绍

本项目是一个高性能的WEB服务器，使用C++实现，项目底层采用了muduo库核心的设计思想，多线程多Reactor的网络模型，并且在这基础上增加了内存池，高效的双缓冲异步日志系统，以及LFU的缓存。

## 开发环境

* linux kernel version5.15.0-113-generic (ubuntu 22.04.6)
* gcc (Ubuntu 11.4.0-1ubuntu1~22.04) 11.4.0
* cmake version 3.22

## 目录结构

```shell
kama-webserver/
├── img/ #存放图片
├── include/ #所有头文件.h位置
├── lib/ #存放共享库
|
├── log/ # 日志管理模块
│ ├── log.cc # 日志实现
├── memory/ # 内存管理模块
│ ├── memory.cc # 内存管理实现
├── src/ # 源代码目录
│ ├── main.cpp # 主程序入口
│ ├── ... # 其他源文件 
|
├── CMakeLists.txt # CMake 构建文件
├── LICENSE # 许可证文件
└── README.md # 项目说明文件
```

## 前置工具准备

安装基本工具

```bash
sudo apt-get update
sudo apt-get install -y wget cmake build-essential unzip git
```

## 编译指令
1. 克隆项目：
```bash
   git clone https://github.com/youngyangyang04/kama-webserver.git
   cd kama-webserver
```

2. 创建构建目录并编译：

```bash
   mkdir build &&
   cd build &&
   cmake .. &&
   make -j ${nproc}
```

3. 在构建完成后，先进入到bin文件

```bash
cd bin
```

4. 启动项目可执行程序main

```bash
./main 
```

**注意**：需要再另外开一个新窗口运行`nc 127.0.0.1 8080`启动我们的客户端，来链接main可执行程序启动的web服务器

## 运行结果
通过运行项目中bin文件下可执行程序main，会出现如下结果：

其中日志文件将存放bin文件下的 `logs` 目录中，每次运行程序时，都会生成新的日志文件，记录程序的运行状态和错误信息。

- 服务器的，运行结果如图

![img](./img/1.png)

- 客户端的，运行结果如图
 
![img](./img/2.png)

**注意**：测试的结果还是采用回声服务器测试,注重架构的实现。

---

### 日志核心内容简单分析：

首先日志结果如图：
![img](./img/3.png)

1. 文件描述符统计

```bash
2025/01/24 17:40:240290 INFO  fd total count:1 - EPollPoller.cc:32
```

- 说明： EPoll 当前管理的文件描述符总数为 1（可能是一个连接的套接字）。

2. 事件触发

```bash
2025/01/24 17:40:454794 INFO  %d events happend1 - EPollPoller.cc:40
2025/01/24 17:40:454852 INFO  channel handleEvent revents:1 - Channel.cc:73
```

- 一个事件发生（events happend1），可能是客户端套接字的关闭事件。
- revents:1 表示触发的事件类型为 EPOLLIN，即对端关闭了连接或者发送了数据。

3. 连接关闭处理

```bash
2025/01/24 17:40:454890 INFO  TcpConnection::handleClose fd=13state=2 - TcpConnection.cc:241
2025/01/24 17:40:454907 INFO  func =>fd13events=0index=1 - EPollPoller.cc:66
2025/01/24 17:40:454929 INFO  Connection DOWN :127.0.0.1:47376 - main.cc:44
```
- TcpConnection::handleClose: 文件描述符 fd=13 的连接关闭，当前状态 state=2（可能表示“已建立连接”状态）。
- Connection DOWN: 与客户端 127.0.0.1:47376 的连接断开。
- events=0: 表示该文件描述符不再监听任何事件。

4. 从服务器中移除连接

```bash 
2025/01/24 17:40:455009 INFO  TcpServer::removeConnectionInLoop [EchoServer] - connection %sEchoServer-127.0.0.1:8080#1 - TcpServer.cc:114
2025/01/24 17:40:455138 INFO  removeChannel fd=13 - EPollPoller.cc:102
```
- TcpServer::removeConnectionInLoop: 服务器内部移除与连接 127.0.0.1:47376 的绑定。
- removeChannel: 从 EPoll 的事件监听列表中移除了文件描述符 fd=13。

5. 资源清理

```bash 
2025/01/24 17:40:455155 INFO  TcpConnection::dtor[EchoServer-127.0.0.1:8080#1]at fd=13state=0 - TcpConnection.cc:58
```
- 调用 TcpConnection 析构函数（dtor），释放连接的相关资源。
- 状态 state=0 表示连接已完全关闭，文件描述符 fd=13 被销毁。


## 功能模块划分

### 网络模块

- **事件轮询与分发模块**：`EventLoop.*`、`Channel.*`、`Poller.*`、`EPollPoller.*`负责事件轮询检测，并实现事件分发处理。`EventLoop`对`Poller`进行轮询，`Poller`底层由`EPollPoller`实现。
- **线程与事件绑定模块**：`Thread.*`、`EventLoopThread.*`、`EventLoopThreadPool.*`绑定线程与事件循环，完成`one loop per thread`模型。
- **网络连接模块**：`TcpServer.*`、`TcpConnection.*`、`Acceptor.*`、`Socket.*`实现`mainloop`对网络连接的响应，并分发到各`subloop`。
- **缓冲区模块**：`Buffer.*`提供自动扩容缓冲区，保证数据有序到达。

### 日志模块

- 日志模块负责记录服务器运行过程中的重要信息，帮助开发者进行调试和性能分析。日志文件存放位于 `bin/logs/` 目录下。

### 内存管理

- 内存管理模块负责动态内存的分配和释放，确保服务器在高负载情况下的稳定性和性能。

### LFU缓存模块
- 用于在缓存容量不足时决定删除哪些内容以释放空间。LFU 的核心思想是优先移除使用频率最低的缓存项。

## 贡献

欢迎任何形式的贡献！请提交问题、建议或代码请求。
