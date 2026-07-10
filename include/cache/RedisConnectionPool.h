#pragma once

#include <hiredis/hiredis.h>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <string>
#include <atomic>
#include "../base/noncopyable.h"

/**
 * @brief Redis 连接池
 *
 * Redis 单线程处理命令，连接池主要省 TCP 握手开销，不是为并行查询。
 * poolSize 建议 2-4，够用即可。
 *
 * borrow() 超时 2 秒返回 nullptr——调用方降级穿透到下一层。
 */
class RedisConnectionPool : noncopyable
{
public:
    RedisConnectionPool(const std::string& host, int port,
                        size_t poolSize = 4);
    ~RedisConnectionPool();

    /// 借一个连接。借出期间独占，shared_ptr 归零自动归还
    std::shared_ptr<redisContext> borrow();

    /// 归还连接（由 shared_ptr 自定义 deleter 自动调用）
    void recycle(redisContext* ctx);

private:
    redisContext* createConnection();
    void          destroyConnection(redisContext* ctx);

    std::string host_;
    int         port_;
    size_t      maxPoolSize_;

    std::queue<redisContext*>  idleConns_;
    std::mutex                 mutex_;
    std::condition_variable    cond_;
    std::atomic<size_t>        activeCount_{0};
};
