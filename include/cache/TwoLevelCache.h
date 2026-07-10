#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <functional>
#include <http/StaticFileCache.h>
#include "LFU.h"
#include "cache/RedisConnectionPool.h"

/**
 * @brief L1(进程私有 LFU) + L2(Redis 共享) 两级缓存协调器
 *
 * 读路径: L1 命中 → 返回 (µs)
 *         L1 miss → L2 命中 → 回填 L1 → 返回 (0.5ms localhost)
 *         L1+L2 miss → 返回 false，调用方读磁盘后回填两级缓存
 *
 * 写路径: L1 put (同步) → L2 SETEX (同步，~0.3ms，可接受)
 *         TTL = 3600s + random(0~300s)，防止雪崩
 *
 * 降级: L2 不可用时穿透到磁盘，服务不挂
 *
 * 穿透保护: 不存在的 key 缓存 30s 空值标记
 *
 * 击穿保护: 同 key 重建加互斥锁，只一个请求去加载
 */
class TwoLevelCache : noncopyable
{
public:
    /**
     * @param redisPool  Redis 连接池
     * @param l1Capacity L1 LFU 缓存容量（默认 200）
     * @param sliceNum   分片数（0 = CPU 核心数）
     */
    TwoLevelCache(RedisConnectionPool* redisPool,
                  size_t l1Capacity = 200, int sliceNum = 0);

    /**
     * @brief 读取缓存（L1 → L2 两级穿透）
     * @return true=命中，entry 被填充
     */
    bool get(const std::string& key, CachedFileEntry& entry);

    /**
     * @brief 写入两级缓存
     * L1 同步写、L2 同步写 SETEX (localhost ~0.3ms)
     */
    void put(const std::string& key, const CachedFileEntry& entry);

    /**
     * @brief 标记 key 不存在（穿透保护：缓存 30s 空值，防止无效请求打到磁盘）
     */
    void markNonexistent(const std::string& key);

    /**
     * @brief 带击穿保护的数据加载。两级缓存 miss 后由调用方传入加载函数，
     *        仅第一个线程执行 loadFunc，其余等待或返回旧值。
     */
    CachedFileEntry getOrLoad(const std::string& key,
                              std::function<bool(CachedFileEntry&)> loadFunc);

    /// 序列化/反序列化 (简单格式: contentType|mtime|fileSize|content)
    static std::string serialize(const CachedFileEntry& e);
    static bool deserialize(const std::string& data, CachedFileEntry& e);

private:
    cache::KHashLfuCache<std::string, CachedFileEntry> l1Cache_;
    RedisConnectionPool* redisPool_;

    // 击穿保护：每个 key 一个重建锁
    std::mutex rebuildMutex_;
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> rebuildLocks_;
};
