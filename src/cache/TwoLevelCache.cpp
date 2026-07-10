#ifdef HAS_REDIS
#include "cache/TwoLevelCache.h"
#include "Logger.h"
#include <cstring>
#include <sstream>
#include <functional>

TwoLevelCache::TwoLevelCache(RedisConnectionPool* redisPool,
                             size_t l1Capacity, int sliceNum)
    : l1Cache_(l1Capacity, sliceNum)
    , redisPool_(redisPool)
{
}

// ========== 序列化 ==========

std::string TwoLevelCache::serialize(const CachedFileEntry& e)
{
    // 格式: contentType|lastModified|fileSize|content
    // 简单分隔符 | ，content 如果含 | 则最后一段
    std::ostringstream oss;
    oss << e.contentType << '|' << e.lastModified << '|' << e.fileSize << '|';
    oss.write(e.content.data(), e.content.size());
    return oss.str();
}

bool TwoLevelCache::deserialize(const std::string& data, CachedFileEntry& e)
{
    // 解析: contentType|lastModified|fileSize|content
    size_t p1 = data.find('|');
    if (p1 == std::string::npos) return false;
    size_t p2 = data.find('|', p1 + 1);
    if (p2 == std::string::npos) return false;
    size_t p3 = data.find('|', p2 + 1);
    if (p3 == std::string::npos) return false;

    e.contentType   = data.substr(0, p1);
    e.lastModified  = std::stoll(data.substr(p1 + 1, p2 - p1 - 1));
    e.fileSize      = std::stoull(data.substr(p2 + 1, p3 - p2 - 1));
    e.content       = data.substr(p3 + 1);
    return true;
}

// ========== 两级读取 ==========

bool TwoLevelCache::get(const std::string& key, CachedFileEntry& entry)
{
    // L1: LFU 内存缓存 (µs 级)
    if (l1Cache_.get(key, entry))
    {
        // 空值标记检查：content 和 fileSize 均为 0 → key 不存在
        if (entry.fileSize == 0 && entry.content.empty())
            return false;
        return true;
    }

    // L2: Redis (localhost ~0.3ms)
    auto redisConn = redisPool_->borrow();
    if (!redisConn)
    {
        // Redis 不可用 → 降级穿透到磁盘（调用方负责）
        return false;
    }

    std::string redisKey = "cache:" + key;
    redisReply* reply = (redisReply*)redisCommand(redisConn.get(),
        "GET %s", redisKey.c_str());

    if (reply && reply->type == REDIS_REPLY_STRING)
    {
        std::string val(reply->str, reply->len);

        // 空值标记
        if (val.empty())
        {
            freeReplyObject(reply);
            return false;
        }

        CachedFileEntry e;
        if (deserialize(val, e))
        {
            // L2 命中 → 回填 L1（提升后续访问到 µs 级）
            l1Cache_.put(key, e);
            entry = std::move(e);
            freeReplyObject(reply);
            return true;
        }
    }
    freeReplyObject(reply);

    // L2 未命中 → 穿透到磁盘
    return false;
}

// ========== 两级写入 ==========

void TwoLevelCache::put(const std::string& key, const CachedFileEntry& entry)
{
    // L1: 同步写 (µs 级)
    l1Cache_.put(key, entry);

    // L2: 同步写 (localhost ~0.3ms，可接受。磁盘 IO 已花 2-5ms)
    auto redisConn = redisPool_->borrow();
    if (!redisConn) return;  // Redis 不可用 → 静默降级，L1 已写入

    std::string redisKey = "cache:" + key;
    std::string val = serialize(entry);

    // TTL 加随机抖动 0~300s，防止批量缓存同时过期 (雪崩保护)
    int ttl = 3600 + static_cast<int>(std::hash<std::string>{}(key) % 300);

    redisReply* reply = (redisReply*)redisCommand(redisConn.get(),
        "SETEX %s %d %b", redisKey.c_str(), ttl, val.data(), val.size());
    if (reply) freeReplyObject(reply);
}

// ========== 穿透保护 ==========

void TwoLevelCache::markNonexistent(const std::string& key)
{
    // L1: 空值标记 (短 TTL 30s，靠 LFU 自然淘汰)
    CachedFileEntry empty;
    empty.lastModified = time(nullptr);
    l1Cache_.put(key, empty);

    // L2: 空字符串标记，TTL 30s
    auto redisConn = redisPool_->borrow();
    if (!redisConn) return;

    std::string redisKey = "cache:" + key;
    redisReply* reply = (redisReply*)redisCommand(redisConn.get(),
        "SETEX %s 30 %b", redisKey.c_str(), "", 0);
    if (reply) freeReplyObject(reply);
}

// ========== 击穿保护：热点重建互斥锁 ==========

CachedFileEntry TwoLevelCache::getOrLoad(
    const std::string& key,
    std::function<bool(CachedFileEntry&)> loadFunc)
{
    // 1. 先查缓存
    CachedFileEntry entry;
    if (get(key, entry))
        return entry;

    // 2. 获取该 key 的重建锁
    std::shared_ptr<std::mutex> keyLock;
    {
        std::lock_guard<std::mutex> lg(rebuildMutex_);
        auto it = rebuildLocks_.find(key);
        if (it != rebuildLocks_.end())
        {
            keyLock = it->second;
        }
        else
        {
            keyLock = std::make_shared<std::mutex>();
            rebuildLocks_[key] = keyLock;
        }
    }

    // 3. 持锁重建
    std::lock_guard<std::mutex> rebuildLock(*keyLock);

    // 4. 双重检查：可能刚刚被其他线程重建好了
    if (get(key, entry))
    {
        // 清理锁，防止 rebuildLocks_ 无限增长
        std::lock_guard<std::mutex> lg(rebuildMutex_);
        rebuildLocks_.erase(key);
        return entry;
    }

    // 5. 加载数据（唯一线程执行磁盘 IO）
    if (loadFunc(entry))
    {
        put(key, entry);  // L1 + L2 回填
    }
    else
    {
        markNonexistent(key);  // 穿透保护
    }

    // 清理锁
    {
        std::lock_guard<std::mutex> lg(rebuildMutex_);
        rebuildLocks_.erase(key);
    }

    return entry;
}

#endif // HAS_REDIS
