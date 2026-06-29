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
#include <http/StaticFileCache.h>

// ========== 配置常量 ==========
static const off_t kRollSize = 1 * 1024 * 1024;            // 日志滚动大小: 1MB
static const char* kStaticRoot = "www";                     // 静态文件根目录
static const size_t kCacheCapacity = 200;                   // LFU 缓存容量(最多缓存文件数)

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

/**
 * @brief 将 URL 路径安全映射为文件系统路径
 *
 * 安全策略:
 *   1. 拒绝包含 ".." 的路径(防目录穿越)
 *   2. 根路径 "/" 映射为 "/index.html"
 *   3. 去掉 URL 中的 query string (调用方已处理，此处做防御)
 *
 * @return 完整的文件系统路径，如 "/www/index.html"
 */
static std::string mapUrlToFilePath(const std::string& urlPath)
{
    // 安全: 拒绝路径穿越
    if (urlPath.find("..") != std::string::npos)
        return "";

    std::string path = urlPath;

    // 根路径默认返回 index.html
    if (path == "/" || path.empty())
        path = "/index.html";

    // 去掉可能的 query string (防御性处理)
    auto queryPos = path.find('?');
    if (queryPos != std::string::npos)
        path = path.substr(0, queryPos);

    return std::string(kStaticRoot) + path;
}

/**
 * @brief 尝试读取文件到内存
 * @return true=读取成功, false=文件不存在或无法读取
 */
static bool readFileToMemory(const std::string& filePath,
                              std::string& content,
                              time_t& mtime,
                              size_t& fileSize)
{
    // 获取文件元信息
    struct stat fileStat;
    if (::stat(filePath.c_str(), &fileStat) != 0)
        return false;

    // 拒绝目录
    if (S_ISDIR(fileStat.st_mode))
        return false;

    // 只读普通文件
    if (!S_ISREG(fileStat.st_mode))
        return false;

    mtime = fileStat.st_mtime;
    fileSize = static_cast<size_t>(fileStat.st_size);

    // 读取文件内容
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open())
        return false;

    std::ostringstream ss;
    ss << file.rdbuf();
    content = ss.str();
    return true;
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

    // ========== 第二步: 初始化内存池和LFU静态文件缓存 ==========
    memoryPool::HashBucket::initMemoryPool();

    // 哈希分片 LFU 缓存，容量200个文件，分片数=CPU核心数
    StaticFileCache fileCache(kCacheCapacity);

    // ========== 第三步: 启动HTTP服务器 ==========
    EventLoop loop;
    InetAddress addr(8080);
    HttpServer server(&loop, addr, "HttpServer");
    server.setThreadNum(3);

    // 注册HTTP路由处理函数
    server.setHttpCallback(
        [&fileCache](const HttpRequest& req, HttpResponse* resp) {
            // -------------------- 静态文件服务 --------------------
            if (req.method() == HttpRequest::kGet || req.method() == HttpRequest::kHead)
            {
                std::string filePath = mapUrlToFilePath(req.path());

                if (filePath.empty())
                {
                    // 路径非法(目录穿越攻击)
                    resp->setStatusCode(HttpResponse::k400BadRequest);
                    resp->setContentType("text/html; charset=utf-8");
                    resp->setBody("<h1>400 Bad Request</h1>");
                    return;
                }

                // 1) 先查 LFU 缓存
                CachedFileEntry entry;
                if (fileCache.get(filePath, entry))
                {
                    // 缓存命中 → 验证文件是否被外部修改(缓存失效)
                    struct stat currentStat;
                    if (::stat(filePath.c_str(), &currentStat) == 0
                        && currentStat.st_mtime == entry.lastModified)
                    {
                        // 缓存有效，直接返回
                        resp->setStatusCode(HttpResponse::k200Ok);
                        resp->setContentType(entry.contentType);
                        resp->setBody(entry.content);
                        return;
                    }
                    // 文件已修改 → 缓存失效，继续走磁盘读取路径
                }

                // 2) 缓存未命中或已过期 → 从磁盘读取
                std::string content;
                time_t mtime = 0;
                size_t fileSize = 0;

                if (readFileToMemory(filePath, content, mtime, fileSize))
                {
                    // 3) 写入缓存 (热点文件自动保留，冷门文件LFU淘汰)
                    std::string mime = StaticFileCache::getMimeType(filePath);
                    fileCache.put(filePath,
                        CachedFileEntry(content, mime, mtime, fileSize));

                    resp->setStatusCode(HttpResponse::k200Ok);
                    resp->setContentType(mime);
                    resp->setBody(content);

                    // HEAD 请求不返回 body
                    if (req.method() == HttpRequest::kHead)
                        resp->setBody(std::string());
                    return;
                }

                // 文件不存在 → 返回 404
                resp->setStatusCode(HttpResponse::k404NotFound);
                resp->setContentType("text/html; charset=utf-8");
                resp->setBody(
                    "<!DOCTYPE html>\r\n"
                    "<html><head><title>404 Not Found</title></head>\r\n"
                    "<body style=\"font-family:sans-serif;text-align:center;padding-top:80px;\">\r\n"
                    "<h1>404 Not Found</h1>\r\n"
                    "<p>The requested URL <code>" + req.path() + "</code> was not found on this server.</p>\r\n"
                    "<hr><small>webserver</small>\r\n"
                    "</body></html>\r\n");
                return;
            }

            // -------------------- 动态路由 (非GET/HEAD请求或API路径) --------------------
            resp->setContentType("text/html; charset=utf-8");

            std::string body;
            body += "<!DOCTYPE html>\r\n";
            body += "<html><head><title>webserver</title></head>\r\n";
            body += "<body>\r\n";
            body += "<h1>Welcome to webserver</h1>\r\n";
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
            body += "<hr><center>webserver</center>\r\n";
            body += "</body></html>\r\n";

            resp->setStatusCode(HttpResponse::k200Ok);
            resp->setBody(body);
        });

    server.start();

    std::cout << "================================================Start Web Server================================================" << std::endl;
    std::cout << "Listening on http://localhost:8080/" << std::endl;
    std::cout << "Static files root: " << kStaticRoot << "/" << std::endl;
    std::cout << "LFU cache capacity: " << kCacheCapacity << " files" << std::endl;
    std::cout << "Memory pool: initialized" << std::endl;
    loop.loop();
    std::cout << "================================================Stop Web Server=================================================" << std::endl;

    // 结束日志
    log.stop();
}
