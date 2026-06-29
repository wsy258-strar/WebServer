#include <string>
#include <libgen.h>

#include <TcpServer.h>
#include <Logger.h>
#include <sys/stat.h>
#include <sstream>
#include <fstream>
#include "AsyncLogging.h"
#include "LFU.h"
#include "memoryPool.h"
#include <http/HttpServer.h>
#include <http/HttpRequest.h>
#include <http/HttpResponse.h>

// 日志文件滚动大小为1MB (1*1024*1024字节)
static const off_t kRollSize = 1*1024*1024;

AsyncLogging* g_asyncLog = NULL;

AsyncLogging* getAsyncLog()
{
    return g_asyncLog;
}

void asyncLog(const char* msg, int len)
{
    AsyncLogging* logging = getAsyncLog();
    if (logging)
    {
        logging->append(msg, len);
    }
}

int main(int argc, char *argv[])
{
    // ========== 第一步: 启动异步日志 ==========
    const std::string LogDir = "logs";
    mkdir(LogDir.c_str(), 0755);

    std::ostringstream LogfilePath;
    LogfilePath << LogDir << "/" << ::basename(argv[0]);
    AsyncLogging log(LogfilePath.str(), kRollSize);
    g_asyncLog = &log;
    Logger::setOutput(asyncLog);
    log.start();

    // ========== 第二步: 初始化内存池和LFU缓存 ==========
    memoryPool::HashBucket::initMemoryPool();

    const int CAPACITY = 5;
    KamaCache::KLfuCache<int, std::string> lfu(CAPACITY);

    // ========== 第三步: 启动HTTP服务器 ==========
    EventLoop loop;
    InetAddress addr(8080);
    HttpServer server(&loop, addr, "HttpServer");
    server.setThreadNum(3);

    // 注册HTTP路由处理函数
    server.setHttpCallback(
        [](const HttpRequest& req, HttpResponse* resp) {
            // 设置默认Content-Type
            resp->setContentType("text/html; charset=utf-8");

            // 构建响应页面，展示请求信息
            std::string body;
            body += "<!DOCTYPE html>\r\n";
            body += "<html><head><title>kama-webserver</title></head>\r\n";
            body += "<body>\r\n";
            body += "<h1>Welcome to kama-webserver</h1>\r\n";
            body += "<h2>Request Info</h2>\r\n";
            body += "<table border=\"1\" cellpadding=\"8\">\r\n";

            // 方法
            body += "<tr><td><b>Method</b></td><td>";
            body += HttpRequest::methodString(req.method());
            body += "</td></tr>\r\n";

            // 路径
            body += "<tr><td><b>Path</b></td><td>";
            body += req.path();
            body += "</td></tr>\r\n";

            // 查询字符串
            if (!req.query().empty())
            {
                body += "<tr><td><b>Query</b></td><td>";
                body += req.query();
                body += "</td></tr>\r\n";

                // 解析后的查询参数
                const auto& params = req.queryParameters();
                if (!params.empty())
                {
                    body += "<tr><td><b>Parsed Query Params</b></td><td>";
                    for (const auto& kv : params)
                    {
                        body += kv.first + " = " + kv.second + "<br>";
                    }
                    body += "</td></tr>\r\n";
                }
            }

            // HTTP版本
            body += "<tr><td><b>Version</b></td><td>";
            body += req.version();
            body += "</td></tr>\r\n";

            // 请求头
            if (!req.headers().empty())
            {
                body += "<tr><td><b>Headers</b></td><td>";
                for (const auto& header : req.headers())
                {
                    body += header.first + ": " + header.second + "<br>";
                }
                body += "</td></tr>\r\n";
            }

            body += "</table>\r\n";
            body += "<hr><center>kama-webserver</center>\r\n";
            body += "</body></html>\r\n";

            resp->setStatusCode(HttpResponse::k200Ok);
            resp->setBody(body);
        });

    server.start();

    std::cout << "================================================Start Web Server================================================" << std::endl;
    std::cout << "Listening on http://localhost:8080/" << std::endl;
    loop.loop();
    std::cout << "================================================Stop Web Server=================================================" << std::endl;

    // 结束日志
    log.stop();
}
