#pragma once

#include <functional>
#include <memory>
#include <future>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <mysql/mysql.h>
#include "../base/noncopyable.h"

/**
 * @brief 异步 DB 工作线程池
 *
 * 每个工作线程借用一条 MySQL 连接 → 执行 SQL 任务 → 归还连接。
 * 线程数 = 连接池大小，避免连接闲置。
 *
 * 关键设计：DB 操作阻塞在独立线程，不阻塞 EventLoop。
 *
 * 用法：
 *   auto conn = pool->borrow();          // 借连接
 *   mysql_query(conn.get(), sql);        // 在独立线程上阻塞执行
 *   // conn 析构 → shared_ptr deleter → recycle() 归还
 */
class DBWorkerPool : noncopyable
{
public:
    /// DB 任务类型：接收 MySQL 连接指针，在独立线程上执行阻塞的 SQL 操作
    using DBTask = std::function<void(std::shared_ptr<MYSQL> conn)>;

    /**
     * @param connPool  MySQL 连接池
     * @param workerCount 工作线程数（建议等于连接池大小）
     */
    DBWorkerPool(class MySQLConnectionPool* connPool, size_t workerCount = 4);
    ~DBWorkerPool();

    /// 投递异步 DB 任务（线程安全，可在任意线程调用）
    void submit(DBTask task);

    /// 同步执行 DB 任务：投递后在当前线程阻塞等待结果，超时 5 秒返回默认值
    /// 注意：仅用于 demo/测试，生产环境应保持异步
    template<typename R>
    R runSync(std::function<R(std::shared_ptr<MYSQL>)> func, R defaultVal = R())
    {
        std::promise<R> prom;
        std::future<R> fut = prom.get_future();
        submit([&prom, func](std::shared_ptr<MYSQL> conn) {
            prom.set_value(func(conn));
        });
        auto status = fut.wait_for(std::chrono::seconds(5));
        if (status == std::future_status::ready)
            return fut.get();
        return defaultVal;
    }

    /// 当前积压的待处理任务数
    size_t pendingCount();

private:
    void run();

    MySQLConnectionPool*               connPool_;// 数据库连接池；工作线程从这里借连接执行 SQL
    std::vector<std::thread>           workers_;// 工作线程集合；每个线程持续从任务队列取任务执行
    std::queue<DBTask>                 tasks_;// 待执行的数据库任务队列，例如“插入用户”“查询订单”
    std::mutex                         mutex_;
    std::condition_variable            cond_;
    std::atomic_bool                   running_{true};
};
