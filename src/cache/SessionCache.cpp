#include "cache/SessionCache.h"

#include <sstream>
#include <vector>

std::string SessionCache::makeKey(const std::string& token)
{
    return "session:" + token;
}

std::string SessionCache::serialize(const Session& session)
{
    std::ostringstream out;
    out << session.id << '|'
        << session.sessionToken << '|'
        << session.userId << '|'
        << session.sceneId << '|'
        << session.status << '|'
        << session.createdAt << '|'
        << session.updatedAt;
    return out.str();
}

bool SessionCache::deserialize(const std::string& data, Session& session)
{
    std::vector<std::string> fields;
    std::istringstream in(data);
    std::string field;
    while (std::getline(in, field, '|'))
        fields.push_back(field);

    if (fields.size() != 7)
        return false;

    try
    {
        session.id = std::stoull(fields[0]);
        session.sessionToken = fields[1];
        session.userId = std::stoull(fields[2]);
        session.sceneId = fields[3];
        session.status = std::stoi(fields[4]);
        session.createdAt = fields[5];
        session.updatedAt = fields[6];
    }
    catch (const std::exception&)
    {
        return false;
    }
    return true;
}

bool SessionCache::get(const std::string& token, Session& session)
{
    if (!redisPool_)
        return false;

    auto conn = redisPool_->borrow();
    if (!conn)
        return false;

    const std::string key = makeKey(token);
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(conn.get(), "GET %s", key.c_str()));
    if (!reply)
        return false;

    const bool valid = reply->type == REDIS_REPLY_STRING
        && deserialize(std::string(reply->str, reply->len), session);
    freeReplyObject(reply);

    if (!valid)
        return false;

    refresh(token);
    return true;
}

bool SessionCache::put(const Session& session)
{
    if (!redisPool_)
        return false;

    auto conn = redisPool_->borrow();
    if (!conn)
        return false;

    const std::string key = makeKey(session.sessionToken);
    const std::string value = serialize(session);
    redisReply* reply = static_cast<redisReply*>(redisCommand(
        conn.get(), "SETEX %s %d %b", key.c_str(), kTtlSeconds,
        value.data(), value.size()));
    if (!reply)
        return false;

    const bool ok = reply->type == REDIS_REPLY_STATUS;
    freeReplyObject(reply);
    return ok;
}

bool SessionCache::refresh(const std::string& token)
{
    if (!redisPool_)
        return false;

    auto conn = redisPool_->borrow();
    if (!conn)
        return false;

    const std::string key = makeKey(token);
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(conn.get(), "EXPIRE %s %d", key.c_str(), kTtlSeconds));
    if (!reply)
        return false;

    const bool ok = reply->type == REDIS_REPLY_INTEGER;
    freeReplyObject(reply);
    return ok;
}

bool SessionCache::remove(const std::string& token)
{
    if (!redisPool_)
        return false;

    auto conn = redisPool_->borrow();
    if (!conn)
        return false;

    const std::string key = makeKey(token);
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(conn.get(), "DEL %s", key.c_str()));
    if (!reply)
        return false;

    const bool ok = reply->type == REDIS_REPLY_INTEGER;
    freeReplyObject(reply);
    return ok;
}
