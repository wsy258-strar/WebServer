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

// ========== MySQL + Redis 集成（可选） ==========
// 安装依赖后启用: sudo apt install libmysqlclient-dev libhiredis-dev
// 未安装时自动回退到纯内存模式
#ifdef HAS_MYSQL
#include <db/MySQLConnectionPool.h>
#include <db/DBWorkerPool.h>
#include <db/SessionDAO.h>
#endif

#ifdef HAS_REDIS
#include <cache/RedisConnectionPool.h>
#include <cache/SessionCache.h>
#include <cache/TwoLevelCache.h>
#endif

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

#ifdef HAS_MYSQL
static std::string formatSessionJson(const Session& session)
{
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"ok\",\"session_id\":%lu,\"token\":\"%s\",\"user_id\":%lu,"
        "\"scene_id\":\"%s\",\"status_code\":%d,\"created_at\":\"%s\",\"updated_at\":\"%s\"}",
        session.id, session.sessionToken.c_str(), session.userId,
        session.sceneId.c_str(), session.status, session.createdAt.c_str(),
        session.updatedAt.c_str());
    return buf;
}
#endif

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

    // ========== 第二步: 初始化内存池和缓存 ==========
    memoryPool::HashBucket::initMemoryPool();

    // L1: 哈希分片 LFU 缓存，容量200个文件，分片数=CPU核心数
    StaticFileCache fileCache(kCacheCapacity);

    // L2 + 持久化（可选，依赖 libmysqlclient-dev libhiredis-dev）
#ifdef HAS_MYSQL
    MySQLConnectionPool::ConnInfo dbInfo{"127.0.0.1", 3306, "ar_app", "Wsy258258!", "webserver"};
    MySQLConnectionPool mysqlPool(dbInfo, 8);
    DBWorkerPool dbWorkerPool(&mysqlPool, 4);
    SessionDAO sessionDAO(&dbWorkerPool);
    LOG_INFO << "MySQL + DBWorkerPool initialized";
#else
    LOG_INFO << "MySQL disabled (compile with -DHAS_MYSQL to enable)";
#endif

#ifdef HAS_REDIS
    RedisConnectionPool redisPool("127.0.0.1", 6379, 4);
    TwoLevelCache twoLevelCache(&redisPool, kCacheCapacity);
    SessionCache sessionCache(&redisPool);
    LOG_INFO << "Redis + TwoLevelCache initialized (L1 LFU + L2 Redis)";
#else
    LOG_INFO << "Redis disabled (compile with -DHAS_REDIS to enable)";
#endif

    // ========== 第三步: 启动HTTP服务器 ==========
    EventLoop loop;
    InetAddress addr(8080);
    HttpServer server(&loop, addr, "HttpServer");
    server.setThreadNum(3);

    // 注册HTTP路由处理函数
    server.setHttpCallback(
        [&fileCache
#ifdef HAS_REDIS
         , &twoLevelCache, &sessionCache
#endif
#ifdef HAS_MYSQL
         , &dbWorkerPool
#endif
        ](const HttpRequest& req, HttpResponse* resp) {
            // -------------------- API 路由（优先于静态文件） --------------------
            // 以 /api/ 开头的请求跳过静态文件处理
            bool isApi = (req.path().find("/api/") == 0);

            // -------------------- 静态文件服务 --------------------
            if (!isApi && (req.method() == HttpRequest::kGet || req.method() == HttpRequest::kHead))
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

                // 1) 先查缓存（L1 LFU，L2 Redis 如果可用）
                CachedFileEntry entry;
                bool cacheHit = false;

#ifdef HAS_REDIS
                // 两级缓存: L1 (µs) → L2 Redis (0.5ms) → 磁盘
                cacheHit = twoLevelCache.get(filePath, entry);
#else
                // 单级缓存: L1 LFU → 磁盘
                cacheHit = fileCache.get(filePath, entry);
#endif
                if (cacheHit)
                {
                    // 缓存命中 → 验证文件是否被外部修改(缓存失效)
                    struct stat currentStat;
                    if (::stat(filePath.c_str(), &currentStat) == 0
                        && currentStat.st_mtime == entry.lastModified)
                    {
                        resp->setStatusCode(HttpResponse::k200Ok);
                        resp->setContentType(entry.contentType);
                        resp->setBody(entry.content);
                        return;
                    }
                }

                // 2) 缓存未命中或已过期 → 从磁盘读取
                std::string content;
                time_t mtime = 0;
                size_t fileSize = 0;

                if (readFileToMemory(filePath, content, mtime, fileSize))
                {
                    std::string mime = StaticFileCache::getMimeType(filePath);
                    CachedFileEntry newEntry(content, mime, mtime, fileSize);

                    // 3) 回填缓存 (L1 + L2)
#ifdef HAS_REDIS
                    twoLevelCache.put(filePath, newEntry);
#else
                    fileCache.put(filePath, newEntry);
#endif

                    resp->setStatusCode(HttpResponse::k200Ok);
                    resp->setContentType(mime);
                    resp->setBody(content);

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

#ifdef HAS_MYSQL
            // -------------------- 用户 & 会话 API --------------------

            // ========== 认证: 注册/登录合一 ==========
            if (req.path() == "/api/auth" && req.method() == HttpRequest::kPost)
            {
                const auto& params = req.queryParameters();
                auto uIt = params.find("username");
                auto pIt = params.find("password");
                if (uIt == params.end() || pIt == params.end()) {
                    resp->setContentType("application/json; charset=utf-8");
                    resp->setBody("{\"error\":\"missing username or password\"}");
                    return;
                }
                std::string username = uIt->second, passwd = pIt->second;
                char body[1024]; uint64_t userId = 0;

                auto* u = dbWorkerPool.runSync<User*>(
                    [&username](std::shared_ptr<MYSQL> conn) -> User* {
                        char sql[256];
                        snprintf(sql, sizeof(sql), "SELECT id, username FROM users WHERE username='%s'", username.c_str());
                        if (mysql_query(conn.get(), sql) != 0) return nullptr;
                        MYSQL_RES* res = mysql_store_result(conn.get());
                        if (!res) return nullptr;
                        MYSQL_ROW row = mysql_fetch_row(res);
                        User* ret = nullptr;
                        if (row) ret = new User{strtoull(row[0], nullptr, 10), row[1], "", ""};
                        mysql_free_result(res);
                        return ret;
                    }, nullptr);

                if (u) {
                    userId = u->id;
                    snprintf(body, sizeof(body),
                        "{\"status\":\"ok\",\"is_new\":false,\"username\":\"%s\",\"user_id\":%lu",
                        u->username.c_str(), userId);
                    delete u;
                } else {
                    bool reg = dbWorkerPool.runSync<bool>(
                        [&username, &passwd, &userId](std::shared_ptr<MYSQL> conn) -> bool {
                            char sql[512];
                            snprintf(sql, sizeof(sql),
                                "INSERT INTO users (username, passwd_hash) VALUES ('%s', '%s')",
                                username.c_str(), passwd.c_str());
                            if (mysql_query(conn.get(), sql) != 0) return false;
                            userId = mysql_insert_id(conn.get());
                            return true;
                        }, false);
                    if (!reg) { resp->setContentType("application/json; charset=utf-8"); resp->setBody("{\"error\":\"register failed\"}"); return; }
                    snprintf(body, sizeof(body),
                        "{\"status\":\"ok\",\"is_new\":true,\"username\":\"%s\",\"user_id\":%lu",
                        username.c_str(), userId);
                }

                char token[128];
                snprintf(token, sizeof(token), "tok-%ld-%04x", time(nullptr), rand() & 0xFFFF);
                uint64_t sessionId = dbWorkerPool.runSync<uint64_t>(
                    [&token, userId](std::shared_ptr<MYSQL> conn) -> uint64_t {
                        char sql[512];
                        snprintf(sql, sizeof(sql),
                            "INSERT INTO sessions (session_token, user_id, scene_id, status) "
                            "VALUES ('%s', %lu, '', 0)", token, userId);
                        if (mysql_query(conn.get(), sql) != 0) return 0;
                        return mysql_insert_id(conn.get());
                    }, 0);
                bool sessOk = sessionId != 0;
#ifdef HAS_REDIS
                if (sessOk) sessionCache.put(Session(sessionId, token, userId, "", 0));
#endif

                char tail[256];
                snprintf(tail, sizeof(tail), ",\"session_token\":\"%s\"}", sessOk ? token : "");
                resp->setContentType("application/json; charset=utf-8");
                resp->setBody(std::string(body) + tail);
                return;
            }

            // ========== 进入场景 ==========
            if (req.path() == "/api/session/enter" && req.method() == HttpRequest::kPost)
            {
                const auto& params = req.queryParameters();
                auto tIt = params.find("token"), sIt = params.find("scene");
                if (tIt == params.end() || sIt == params.end()) {
                    resp->setContentType("application/json; charset=utf-8");
                    resp->setBody("{\"error\":\"missing token or scene\"}"); return;
                }
                std::string token = tIt->second, sceneId = sIt->second;
                bool ok = dbWorkerPool.runSync<bool>(
                    [&token, &sceneId](std::shared_ptr<MYSQL> conn) -> bool {
                        char sql[512];
                        snprintf(sql, sizeof(sql),
                            "UPDATE sessions SET scene_id='%s', status=1 "
                            "WHERE session_token='%s'",
                            sceneId.c_str(), token.c_str());
                        int ret = mysql_query(conn.get(), sql);
                        if (ret != 0) {
                            LOG_ERROR << "Enter scene SQL failed: " << mysql_error(conn.get());
                        }
                        return ret == 0;
                    }, false);
#ifdef HAS_REDIS
                if (ok) {
                    auto session = dbWorkerPool.runSync<std::shared_ptr<Session>>(
                        [&token](std::shared_ptr<MYSQL> conn) -> std::shared_ptr<Session> {
                            char sql[512];
                            snprintf(sql, sizeof(sql),
                                "SELECT id, session_token, user_id, scene_id, status, created_at, updated_at "
                                "FROM sessions WHERE session_token='%s' ORDER BY id DESC LIMIT 1",
                                token.c_str());
                            if (mysql_query(conn.get(), sql) != 0) return nullptr;
                            MYSQL_RES* res = mysql_store_result(conn.get());
                            if (!res) return nullptr;
                            MYSQL_ROW row = mysql_fetch_row(res);
                            std::shared_ptr<Session> result;
                            if (row) result = std::make_shared<Session>(
                                strtoull(row[0], nullptr, 10), row[1],
                                strtoull(row[2], nullptr, 10), row[3], atoi(row[4]),
                                row[5] ? row[5] : "", row[6] ? row[6] : "");
                            mysql_free_result(res);
                            return result;
                        }, std::shared_ptr<Session>());
                    if (session) sessionCache.put(*session);
                }
#endif
                resp->setContentType("application/json; charset=utf-8");
                if (ok) resp->setBody("{\"status\":\"ok\",\"scene_id\":\"" + sceneId + "\"}");
                else    resp->setBody("{\"error\":\"enter scene failed\"}");
                return;
            }

            // ========== 退出场景 ==========
            if (req.path() == "/api/session/exit" && req.method() == HttpRequest::kPost)
            {
                const auto& params = req.queryParameters();
                auto tIt = params.find("token");
                if (tIt == params.end()) {
                    resp->setContentType("application/json; charset=utf-8");
                    resp->setBody("{\"error\":\"missing token\"}"); return;
                }
                std::string token = tIt->second;
                // 先查当前状态
                int currentStatus = dbWorkerPool.runSync<int>(
                    [&token](std::shared_ptr<MYSQL> conn) -> int {
                        char sql[512];
                        snprintf(sql, sizeof(sql),
                            "SELECT status FROM sessions WHERE session_token='%s'",
                            token.c_str());
                        if (mysql_query(conn.get(), sql) != 0) return -1;
                        MYSQL_RES* res = mysql_store_result(conn.get());
                        if (!res) return -1;
                        MYSQL_ROW row = mysql_fetch_row(res);
                        int s = (row && row[0]) ? atoi(row[0]) : -1;
                        mysql_free_result(res);
                        return s;
                    }, -1);

                if (currentStatus == 0) {
                    resp->setContentType("application/json; charset=utf-8");
                    resp->setBody("{\"error\":\"already exited\"}");
                    return;
                }

                bool ok = dbWorkerPool.runSync<bool>(
                    [&token](std::shared_ptr<MYSQL> conn) -> bool {
                        char sql[512];
                        snprintf(sql, sizeof(sql),
                            "UPDATE sessions SET scene_id='', status=0 "
                            "WHERE session_token='%s'", token.c_str());
                        return mysql_query(conn.get(), sql) == 0;
                    }, false);
#ifdef HAS_REDIS
                if (ok) sessionCache.remove(token);
#endif
                resp->setContentType("application/json; charset=utf-8");
                if (ok) resp->setBody("{\"status\":\"ok\"}");
                else    resp->setBody("{\"error\":\"exit failed\"}");
                return;
            }

            // ========== 查询会话 ==========
            if (req.path() == "/api/session" && req.method() == HttpRequest::kGet)
            {
                const auto& params = req.queryParameters();
                auto it = params.find("token");
                std::string token = (it != params.end()) ? it->second : "";
                if (token.empty()) {
                    resp->setContentType("application/json; charset=utf-8");
                    resp->setBody("{\"error\":\"missing token\"}"); return;
                }
#ifdef HAS_REDIS
                Session cached;
                if (sessionCache.get(token, cached)) {
                    resp->setContentType("application/json; charset=utf-8");
                    resp->setBody(formatSessionJson(cached));
                    return;
                }
#endif
                using SessionPtr = std::shared_ptr<Session>;
                auto result = dbWorkerPool.runSync<SessionPtr>(
                    [&token](std::shared_ptr<MYSQL> conn) -> SessionPtr {
                        char sql[512];
                        snprintf(sql, sizeof(sql),
                            "SELECT id, session_token, user_id, scene_id, status, created_at, updated_at "
                            "FROM sessions WHERE session_token='%s' ORDER BY id DESC LIMIT 1",
                            token.c_str());
                        if (mysql_query(conn.get(), sql) != 0) return nullptr;
                        MYSQL_RES* res = mysql_store_result(conn.get());
                        if (!res) return nullptr;
                        MYSQL_ROW row = mysql_fetch_row(res);
                        SessionPtr ret;
                        if (row) {
                            ret = std::make_shared<Session>(
                                strtoull(row[0], nullptr, 10), row[1],
                                strtoull(row[2], nullptr, 10), row[3], atoi(row[4]),
                                row[5] ? row[5] : "", row[6] ? row[6] : "");
                        }
                        mysql_free_result(res);
                        return ret;
                    }, SessionPtr());
                resp->setContentType("application/json; charset=utf-8");
                if (result) {
#ifdef HAS_REDIS
                    sessionCache.put(*result);
#endif
                    resp->setBody(formatSessionJson(*result));
                }
                else        resp->setBody("{\"status\":\"not_found\"}");
                return;
            }
#endif

            // -------------------- 动态路由 (非GET/HEAD请求) --------------------
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

#ifdef HAS_MYSQL
    // 注册保活定时器: 每 5 分钟 ping 空闲 MySQL 连接
    loop.runEvery(300.0, mysqlPool.makeKeepAliveFunc());
#endif

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
