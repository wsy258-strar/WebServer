#pragma once

#include <mysql/mysql.h>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <string>
#include <chrono>
#include <atomic>
#include <functional>
#include "../base/noncopyable.h"

/**
 * @brief MySQL 连接池
 *
 * 复用 TCP 连接，避免每次 SQL 操作都重新握手+鉴权(~10-20ms)。
 * 连接生命周期：borrow() 借出 → 使用 → shared_ptr 引用归零 → recycle() 归还。
 *
 * 池大小建议：4-8（与 DBWorkerPool 线程数匹配，避免连接闲置）。
 * 保活：后台定时器每 5 分钟 ping 空闲超过 4 分钟的连接。
 *
 * 降级：borrow() 超时返回 nullptr，调用方自行穿透到下一层或返回错误。
 */
class MySQLConnectionPool : noncopyable
{
public:
    struct ConnInfo
    {
        std::string host;
        int         port;
        std::string user;
        std::string passwd;
        std::string db;
    };

    MySQLConnectionPool(const ConnInfo& info, size_t poolSize = 8);
    ~MySQLConnectionPool();

    /// 借一条连接。超时 3 秒未拿到返回 nullptr（降级信号），接池在 3 秒内借不到可用连接时，
    // 不再继续阻塞或报致命错误，而是返回 nullptr，让上层改走一个较弱的兜底方案。
    std::shared_ptr<MYSQL> borrow();

    /// 归还连接（由 shared_ptr 自定义 deleter 自动调用，通常不需手动调用）
    void recycle(MYSQL* conn);

    /// 当前池中活跃连接数
    size_t activeCount() const { return activeCount_; }

    /// 注册保活回调。调用方通过现有 TimerQueue::runEvery 驱动保活。
    /// 返回值为需保活的函数对象，无锁、可在任意线程调用。
    std::function<void()> makeKeepAliveFunc();

private:
    struct PooledConn
    {
        MYSQL* conn;
        std::chrono::steady_clock::time_point lastUsed;
    };

    MYSQL* createConnection();
    void   destroyConnection(MYSQL* conn);
    bool   ping(MYSQL* conn);

    ConnInfo        info_;
    size_t          maxPoolSize_;

    std::queue<PooledConn> idleConns_;
    std::mutex             mutex_;
    std::condition_variable cond_;
    std::atomic<size_t>    activeCount_{0};
    std::atomic_bool       running_{true};
};
