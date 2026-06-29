#pragma once

#include <functional>
#include <string>
#include <memory>

#include <net/TcpServer.h>
#include <net/InetAddress.h>
#include <net/Buffer.h>
#include <net/Callbacks.h>
#include <http/HttpRequest.h>
#include <http/HttpResponse.h>
#include <base/noncopyable.h>

/**
 * @brief HTTP服务器外观类
 *
 * 封装TcpServer，提供HTTP协议层处理。
 * 使用方式(类似EchoServer):
 *   HttpServer server(&loop, addr, "HttpServer");
 *   server.setHttpCallback([](const HttpRequest& req, HttpResponse* resp) { ... });
 *   server.setThreadNum(3);
 *   server.start();
 */
class HttpServer : noncopyable
{
public:
    using HttpCallback = std::function<void(const HttpRequest&, HttpResponse*)>;

    HttpServer(EventLoop* loop,
               const InetAddress& listenAddr,
               const std::string& name);

    /// 设置HTTP请求处理回调(路由)
    void setHttpCallback(const HttpCallback& cb) { httpCallback_ = cb; }

    /// 设置工作线程数(subLoop数量)
    void setThreadNum(int numThreads) { server_.setThreadNum(numThreads); }

    /// 可选的连接事件回调(connect/disconnect)，在HTTP处理之外额外通知
    void setConnectionCallback(const ConnectionCallback& cb)
    { connectionCallback_ = cb; }

    /// 启动HTTP服务器(开始监听)
    void start() { server_.start(); }

    /// 访问底层TcpServer(高级用法)
    TcpServer& tcpServer() { return server_; }

private:
    void onConnection(const TcpConnectionPtr& conn);
    void onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp receiveTime);
    void onRequest(const TcpConnectionPtr& conn, const HttpRequest& req);

    TcpServer server_;
    HttpCallback httpCallback_;
    ConnectionCallback connectionCallback_;
};
