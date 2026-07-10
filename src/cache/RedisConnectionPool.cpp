#ifdef HAS_REDIS
#include "cache/RedisConnectionPool.h"
#include "Logger.h"

RedisConnectionPool::RedisConnectionPool(const std::string& host, int port,
                                         size_t poolSize)
    : host_(host), port_(port), maxPoolSize_(poolSize)
{
    for (size_t i = 0; i < poolSize; ++i)
    {
        redisContext* ctx = createConnection();
        if (ctx)
        {
            idleConns_.push(ctx);
            activeCount_++;
        }
    }
    LOG_INFO << "Redis pool created: " << activeCount_.load() << "/" << poolSize;
}

RedisConnectionPool::~RedisConnectionPool()
{
    std::lock_guard<std::mutex> lock(mutex_);
    while (!idleConns_.empty())
    {
        destroyConnection(idleConns_.front());
        idleConns_.pop();
    }
}

redisContext* RedisConnectionPool::createConnection()
{
    // hiredis 同步连接，设置 2 秒超时
    struct timeval timeout = {2, 0};
    redisContext* ctx = redisConnectWithTimeout(host_.c_str(), port_, timeout);
    if (!ctx || ctx->err)
    {
        LOG_ERROR << "Redis connect failed: "
                   << (ctx ? ctx->errstr : "OOM");
        if (ctx) redisFree(ctx);
        return nullptr;
    }
    // 管道模式：多个命令批量发送，减少 RTT
    // （可选，默认不使用）
    return ctx;
}

void RedisConnectionPool::destroyConnection(redisContext* ctx)
{
    if (ctx)
    {
        redisFree(ctx);
        activeCount_--;
    }
}

std::shared_ptr<redisContext> RedisConnectionPool::borrow()
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (!idleConns_.empty())
    {
        auto ctx = idleConns_.front();
        idleConns_.pop();
        return {ctx, [this](redisContext* c) { this->recycle(c); }};
    }

    if (activeCount_ < maxPoolSize_)
    {
        activeCount_++;
        lock.unlock();
        redisContext* ctx = createConnection();
        if (ctx)
            return {ctx, [this](redisContext* c) { this->recycle(c); }};
        activeCount_--;
        return nullptr;
    }

    // 池满 → 等待，最多 2 秒
    if (!cond_.wait_for(lock, std::chrono::seconds(2),
                        [this] { return !idleConns_.empty(); }))
    {
        LOG_WARN << "Redis pool exhausted, borrow timeout";
        return nullptr;  // 降级：穿透到磁盘
    }

    auto ctx = idleConns_.front();
    idleConns_.pop();
    return {ctx, [this](redisContext* c) { this->recycle(c); }};
}

void RedisConnectionPool::recycle(redisContext* ctx)
{
    if (!ctx) return;

    // 检查连接健康状态
    if (ctx->err)
    {
        LOG_WARN << "Redis connection error during recycle: " << ctx->errstr;
        destroyConnection(ctx);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        idleConns_.push(ctx);
    }
    cond_.notify_one();
}

#endif // HAS_REDIS
