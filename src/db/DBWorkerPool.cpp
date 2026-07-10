#include "db/DBWorkerPool.h"
#include "db/MySQLConnectionPool.h"
#include "Logger.h"

DBWorkerPool::DBWorkerPool(MySQLConnectionPool* connPool, size_t workerCount)
    : connPool_(connPool)
{
    for (size_t i = 0; i < workerCount; ++i)
    {
        workers_.emplace_back(&DBWorkerPool::run, this);
    }
    LOG_INFO << "DBWorkerPool started with " << workerCount << " workers";
}

DBWorkerPool::~DBWorkerPool()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    cond_.notify_all();

    for (auto& w : workers_)
    {
        if (w.joinable()) w.join();
    }
}

void DBWorkerPool::submit(DBTask task)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push(std::move(task));
    }
    cond_.notify_one();
}

size_t DBWorkerPool::pendingCount()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

void DBWorkerPool::run()
{
    while (running_)
    {
        DBTask task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait(lock, [this] {
                return !tasks_.empty() || !running_;
            });

            if (!running_ && tasks_.empty()) return;

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        // 借连接 → 执行 → 自动归还（shared_ptr deleter）
        auto conn = connPool_->borrow();
        if (!conn)
        {
            LOG_ERROR << "DBWorker: failed to borrow connection, task dropped";
            continue;  // 降级：跳过此任务，不会崩溃
        }

        task(conn);
        // conn 析构 → recycle() 归还
    }
}
