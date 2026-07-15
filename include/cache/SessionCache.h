#pragma once

#include <string>

#include "cache/RedisConnectionPool.h"
#include "db/SessionDAO.h"

class SessionCache : noncopyable
{
public:
    static const int kTtlSeconds = 1800;

    explicit SessionCache(RedisConnectionPool* redisPool)
        : redisPool_(redisPool) {}

    bool get(const std::string& token, Session& session);
    bool put(const Session& session);
    bool refresh(const std::string& token);
    bool remove(const std::string& token);

    static std::string serialize(const Session& session);
    static bool deserialize(const std::string& data, Session& session);

private:
    static std::string makeKey(const std::string& token);

    RedisConnectionPool* redisPool_;
};
