#include <http/HttpServer.h>
#include <http/HttpContext.h>
#include <Logger.h>

HttpServer::HttpServer(EventLoop* loop,
                       const InetAddress& listenAddr,
                       const std::string& name)
    : server_(loop, listenAddr, name)
{
    server_.setConnectionCallback(
        std::bind(&HttpServer::onConnection, this, std::placeholders::_1));
    server_.setMessageCallback(
        std::bind(&HttpServer::onMessage, this,
                  std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
}

// ========== 连接回调 ==========

void HttpServer::onConnection(const TcpConnectionPtr& conn)
{
    if (conn->connected())
    {
        LOG_INFO << "HTTP connection UP   : " << conn->peerAddress().toIpPort();
    }
    else
    {
        LOG_INFO << "HTTP connection DOWN : " << conn->peerAddress().toIpPort();
        // HttpContext 随 TcpConnection 生命周期自动销毁，无需手动清理
    }

    // 传递给用户自定义的连接回调
    if (connectionCallback_)
    {
        connectionCallback_(conn);
    }
}

// ========== 消息回调 (核心分发逻辑) ==========

void HttpServer::onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp receiveTime)
{
    // 获取该连接的HttpContext (每连接独立，无线程竞争)
    HttpContext& context = conn->getContext();//getContext()返回的是HTTP 协议层上下文(每连接独立的解析状态，避免多线程竞争)

    // 增量解析
    if (!context.parseRequest(buf, receiveTime))
    {
        // 解析失败 → 400 Bad Request
        LOG_WARN << "HTTP parse error from " << conn->peerAddress().toIpPort();
        HttpResponse resp = HttpResponse::makeErrorResponse(
            HttpResponse::k400BadRequest, true, "Failed to parse request");
        Buffer respBuf;
        resp.appendToBuffer(&respBuf);
        conn->send(respBuf.retrieveAllAsString());
        conn->shutdown();
        return;
    }

    // 如果解析出一个完整请求，分发处理
    if (context.gotAll())
    {
        onRequest(conn, context.request());
        context.reset(); // 准备接收同一连接的下一个请求(keep-alive)
    }
    // 否则，等待更多数据(下次onMessage继续喂入同一个context)
}

// ========== 请求分发 ==========
//主要做两件事：决定是否保持连接，然后回调用户。

void HttpServer::onRequest(const TcpConnectionPtr& conn, const HttpRequest& req)
{
    // 决定是否关闭连接
    const std::string& connectionHeader = req.getHeader("connection");
    bool close = false;

    if (req.version() == "HTTP/1.0")
    {
        // HTTP/1.0 默认关闭，除非 Connection: Keep-Alive
        close = (connectionHeader != "keep-alive");
    }
    else // HTTP/1.1 或更高
    {
        // HTTP/1.1 默认keep-alive，除非 Connection: close
        close = (connectionHeader == "close");
    }

    HttpResponse response(close);
    response.setStatusCode(HttpResponse::k200Ok);

    if (httpCallback_)
    {
        httpCallback_(req, &response);
    }

    // 序列化并发送
    Buffer respBuf;
    response.appendToBuffer(&respBuf);
    conn->send(respBuf.retrieveAllAsString());

    // 如果需要关闭连接
    if (response.closeConnection())
    {
        conn->shutdown();
    }
}
