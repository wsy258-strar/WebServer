#include "db/MySQLConnectionPool.h"
#include "Logger.h"
#include <cstring>

MySQLConnectionPool::MySQLConnectionPool(const ConnInfo& info, size_t poolSize)
    : info_(info)
    , maxPoolSize_(poolSize)
{
    // 预创建 2 条热连接，其余按需创建
    for (size_t i = 0; i < 2 && i < maxPoolSize_; ++i)
    {
        MYSQL* conn = createConnection();
        if (conn)
        {
            idleConns_.push({conn, std::chrono::steady_clock::now()});
            activeCount_++;
        }
    }
}

MySQLConnectionPool::~MySQLConnectionPool()
{
    running_ = false;
    cond_.notify_all();

    std::lock_guard<std::mutex> lock(mutex_);
    while (!idleConns_.empty())
    {
        destroyConnection(idleConns_.front().conn);
        idleConns_.pop();
    }
}

MYSQL* MySQLConnectionPool::createConnection()
{
    MYSQL* conn = mysql_init(nullptr);
    if (!conn)
    {
        LOG_ERROR << "mysql_init failed";
        return nullptr;
    }

    // 设置读写超时（不用 MYSQL_OPT_RECONNECT，新版 MySQL 已废弃）
    // 连接断开时 borrow/recycle 会检测到并重新连接
    unsigned int timeout = 5;
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &timeout);

    if (!mysql_real_connect(conn, info_.host.c_str(), info_.user.c_str(),
                             info_.passwd.c_str(), info_.db.c_str(),
                             info_.port, nullptr, 0))
    {
        LOG_ERROR << "mysql_real_connect failed: " << mysql_error(conn);
        mysql_close(conn);
        return nullptr;
    }

    // 设置 UTF-8 字符集
    mysql_set_character_set(conn, "utf8mb4");
    return conn;
}

void MySQLConnectionPool::destroyConnection(MYSQL* conn)
{
    if (conn)
    {
        mysql_close(conn);
        activeCount_--;
    }
}

bool MySQLConnectionPool::ping(MYSQL* conn)
{
    return mysql_ping(conn) == 0;
}

std::shared_ptr<MYSQL> MySQLConnectionPool::borrow()
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (!idleConns_.empty())
    {
        auto pooled = idleConns_.front();
        idleConns_.pop();
        return {pooled.conn, [this](MYSQL* c) { this->recycle(c); }};
    }

    // 池未满 → 新建连接（在锁外创建，避免阻塞其他 borrow 调用）
    if (activeCount_ < maxPoolSize_)
    {
        activeCount_++;
        lock.unlock();

        MYSQL* conn = createConnection();
        if (conn)
        {
            return {conn, [this](MYSQL* c) { this->recycle(c); }};
        }

        // 创建失败，回退计数
        activeCount_--;
        return nullptr;
    }

    // 池已满 → 等待空闲连接，最多 3 秒
    if (!cond_.wait_for(lock, std::chrono::seconds(3),
                        [this] { return !idleConns_.empty(); }))
    {
        LOG_WARN << "MySQL pool exhausted, borrow timeout after 3s";
        return nullptr;  // 降级信号
    }

    auto pooled = idleConns_.front();
    idleConns_.pop();
    return {pooled.conn, [this](MYSQL* c) { this->recycle(c); }};
}

void MySQLConnectionPool::recycle(MYSQL* conn)
{
    if (!conn) return;

    // 简单健康检查：连接是否仍然有效
    if (mysql_ping(conn) != 0)
    {
        LOG_WARN << "MySQL connection lost during recycle, destroying";
        destroyConnection(conn);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        idleConns_.push({conn, std::chrono::steady_clock::now()});
    }
    cond_.notify_one();
}

std::function<void()> MySQLConnectionPool::makeKeepAliveFunc()
{
    // 捕获 this 裸指针：保活函数生命周期由调用方保证不晚于池析构
    auto* self = this;
    return [self]() {
        std::lock_guard<std::mutex> lock(self->mutex_);
        auto now = std::chrono::steady_clock::now();
        std::queue<PooledConn> alive;

        while (!self->idleConns_.empty())
        {
            auto c = self->idleConns_.front();
            self->idleConns_.pop();
            //计算连接 c 距离上次使用已经空闲了多少整秒
            auto idleSec = std::chrono::duration_cast<std::chrono::seconds>(
                               now - c.lastUsed).count();
            //判断空闲连接是否应被回收
            if (idleSec > 240)  // 空闲超 4 分钟 → ping
            {
                if (self->ping(c.conn))
                {
                    c.lastUsed = now;
                    alive.push(c);
                }
                else
                {
                    LOG_WARN << "MySQL keep-alive ping failed, destroying connection";
                    self->destroyConnection(c.conn);
                }
            }
            else
            {
                alive.push(c);
            }
        }
        self->idleConns_ = std::move(alive);
    };
}
