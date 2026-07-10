#include "db/SessionDAO.h"
#include "db/DBWorkerPool.h"
#include "db/MySQLConnectionPool.h"
#include "Logger.h"
#include <cstring>
#include <mysql/mysql.h>

// ========== 工具函数 ==========

namespace {

/// 安全地从结果集取字符串字段，NULL 返回空串
std::string safeStr(const char* s) { return s ? std::string(s) : std::string(); }

} // anonymous

// ========== 用户操作 ==========

void SessionDAO::findUserById(uint64_t userId,
                               std::function<void(User*)> callback)
{
    dbPool_->submit([userId, callback](std::shared_ptr<MYSQL> conn) {
        MYSQL_STMT* stmt = mysql_stmt_init(conn.get());
        const char* sql = "SELECT id, username, passwd_hash, created_at "
                          "FROM users WHERE id = ?";
        if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0)
        {
            mysql_stmt_close(stmt);
            callback(nullptr);
            return;
        }

        MYSQL_BIND bind[1];
        memset(bind, 0, sizeof(bind));
        bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
        uint64_t uid = userId;  // 非 const 副本，满足 MySQL C API
        bind[0].buffer = &uid;
        mysql_stmt_bind_param(stmt, bind);
        mysql_stmt_execute(stmt);

        uint64_t id = 0;
        char username[256] = {0}, passwd[256] = {0}, created[64] = {0};
        MYSQL_BIND result[4];
        memset(result, 0, sizeof(result));
        result[0].buffer_type = MYSQL_TYPE_LONGLONG;
        result[0].buffer = &id;
        result[1].buffer_type = MYSQL_TYPE_STRING;
        result[1].buffer = username;
        result[1].buffer_length = sizeof(username);
        result[2].buffer_type = MYSQL_TYPE_STRING;
        result[2].buffer = passwd;
        result[2].buffer_length = sizeof(passwd);
        result[3].buffer_type = MYSQL_TYPE_STRING;
        result[3].buffer = created;
        result[3].buffer_length = sizeof(created);
        mysql_stmt_bind_result(stmt, result);

        if (mysql_stmt_fetch(stmt) == 0)
        {
            auto* u = new User;
            u->id = id;
            u->username = username;
            u->passwdHash = passwd;
            u->createdAt = created;
            mysql_stmt_close(stmt);
            callback(u);
            delete u;
            return;
        }
        mysql_stmt_close(stmt);
        callback(nullptr);
    });
}

void SessionDAO::findUserByUsername(const std::string& username,
                                     std::function<void(User*)> callback)
{
    dbPool_->submit([username, callback](std::shared_ptr<MYSQL> conn) {
        MYSQL_STMT* stmt = mysql_stmt_init(conn.get());
        const char* sql = "SELECT id, username, passwd_hash, created_at "
                          "FROM users WHERE username = ?";
        if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0)
        {
            mysql_stmt_close(stmt);
            callback(nullptr);
            return;
        }

        MYSQL_BIND bind[1];
        memset(bind, 0, sizeof(bind));
        unsigned long nameLen = username.size();
        bind[0].buffer_type = MYSQL_TYPE_STRING;
        bind[0].buffer = const_cast<char*>(username.c_str());
        bind[0].buffer_length = nameLen;
        bind[0].length = &nameLen;
        mysql_stmt_bind_param(stmt, bind);
        mysql_stmt_execute(stmt);

        uint64_t id = 0;
        char user[256] = {0}, passwd[256] = {0}, created[64] = {0};
        MYSQL_BIND result[4];
        memset(result, 0, sizeof(result));
        result[0].buffer_type = MYSQL_TYPE_LONGLONG;
        result[0].buffer = &id;
        result[1].buffer_type = MYSQL_TYPE_STRING;
        result[1].buffer = user;
        result[1].buffer_length = sizeof(user);
        result[2].buffer_type = MYSQL_TYPE_STRING;
        result[2].buffer = passwd;
        result[2].buffer_length = sizeof(passwd);
        result[3].buffer_type = MYSQL_TYPE_STRING;
        result[3].buffer = created;
        result[3].buffer_length = sizeof(created);
        mysql_stmt_bind_result(stmt, result);

        if (mysql_stmt_fetch(stmt) == 0)
        {
            auto* u = new User;
            u->id = id;
            u->username = user;
            u->passwdHash = passwd;
            u->createdAt = created;
            mysql_stmt_close(stmt);
            callback(u);
            delete u;
            return;
        }
        mysql_stmt_close(stmt);
        callback(nullptr);
    });
}

void SessionDAO::createUser(const std::string& username,
                             const std::string& passwdHash,
                             std::function<void(uint64_t)> callback)
{
    dbPool_->submit([username, passwdHash, callback](std::shared_ptr<MYSQL> conn) {
        MYSQL_STMT* stmt = mysql_stmt_init(conn.get());
        const char* sql = "INSERT INTO users (username, passwd_hash) VALUES (?, ?)";
        if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0)
        {
            mysql_stmt_close(stmt);
            callback(0);
            return;
        }

        MYSQL_BIND bind[2];
        memset(bind, 0, sizeof(bind));
        unsigned long nameLen = username.size(), pwLen = passwdHash.size();
        bind[0].buffer_type = MYSQL_TYPE_STRING;
        bind[0].buffer = const_cast<char*>(username.c_str());
        bind[0].buffer_length = nameLen;
        bind[0].length = &nameLen;
        bind[1].buffer_type = MYSQL_TYPE_STRING;
        bind[1].buffer = const_cast<char*>(passwdHash.c_str());
        bind[1].buffer_length = pwLen;
        bind[1].length = &pwLen;
        mysql_stmt_bind_param(stmt, bind);

        if (mysql_stmt_execute(stmt) == 0)
            callback(mysql_stmt_insert_id(stmt));
        else
            callback(0);
        mysql_stmt_close(stmt);
    });
}

// ========== 会话操作 ==========

void SessionDAO::findSessionByToken(const std::string& token,
                                     std::function<void(Session*)> callback)
{
    dbPool_->submit([token, callback](std::shared_ptr<MYSQL> conn) {
        MYSQL_STMT* stmt = mysql_stmt_init(conn.get());
        const char* sql = "SELECT id, session_token, user_id, scene_id, status, "
                          "created_at, updated_at FROM sessions "
                          "WHERE session_token = ? AND status = 1";
        if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0)
        {
            mysql_stmt_close(stmt);
            callback(nullptr);
            return;
        }

        MYSQL_BIND bind[1];
        memset(bind, 0, sizeof(bind));
        unsigned long tkLen = token.size();
        bind[0].buffer_type = MYSQL_TYPE_STRING;
        bind[0].buffer = const_cast<char*>(token.c_str());
        bind[0].buffer_length = tkLen;
        bind[0].length = &tkLen;
        mysql_stmt_bind_param(stmt, bind);
        mysql_stmt_execute(stmt);

        uint64_t id = 0, userId = 0;
        char sToken[256] = {0}, scene[128] = {0};
        int status = 0;
        char cAt[64] = {0}, uAt[64] = {0};
        MYSQL_BIND result[7];
        memset(result, 0, sizeof(result));
        result[0].buffer_type = MYSQL_TYPE_LONGLONG; result[0].buffer = &id;
        result[1].buffer_type = MYSQL_TYPE_STRING;   result[1].buffer = sToken;
        result[1].buffer_length = sizeof(sToken);
        result[2].buffer_type = MYSQL_TYPE_LONGLONG; result[2].buffer = &userId;
        result[3].buffer_type = MYSQL_TYPE_STRING;   result[3].buffer = scene;
        result[3].buffer_length = sizeof(scene);
        result[4].buffer_type = MYSQL_TYPE_LONG;     result[4].buffer = &status;
        result[5].buffer_type = MYSQL_TYPE_STRING;   result[5].buffer = cAt;
        result[5].buffer_length = sizeof(cAt);
        result[6].buffer_type = MYSQL_TYPE_STRING;   result[6].buffer = uAt;
        result[6].buffer_length = sizeof(uAt);
        mysql_stmt_bind_result(stmt, result);

        if (mysql_stmt_fetch(stmt) == 0)
        {
            auto* s = new Session;
            s->id = id; s->sessionToken = sToken; s->userId = userId;
            s->sceneId = scene; s->status = status;
            s->createdAt = cAt; s->updatedAt = uAt;
            mysql_stmt_close(stmt);
            callback(s);
            delete s;
            return;
        }
        mysql_stmt_close(stmt);
        callback(nullptr);
    });
}

void SessionDAO::createSession(uint64_t userId,
                                const std::string& token,
                                const std::string& sceneId,
                                std::function<void(uint64_t)> callback)
{
    dbPool_->submit([userId, token, sceneId, callback](std::shared_ptr<MYSQL> conn) {
        MYSQL_STMT* stmt = mysql_stmt_init(conn.get());
        const char* sql = "INSERT INTO sessions (session_token, user_id, scene_id) "
                          "VALUES (?, ?, ?)";
        if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0)
        {
            mysql_stmt_close(stmt);
            callback(0);
            return;
        }

        MYSQL_BIND bind[3];
        memset(bind, 0, sizeof(bind));
        unsigned long tkLen = token.size(), scLen = sceneId.size();
        bind[0].buffer_type = MYSQL_TYPE_STRING;
        bind[0].buffer = const_cast<char*>(token.c_str());
        bind[0].buffer_length = tkLen;
        bind[0].length = &tkLen;
        bind[1].buffer_type = MYSQL_TYPE_LONGLONG;
        uint64_t uid_copy = userId;
        bind[1].buffer = &uid_copy;
        bind[2].buffer_type = MYSQL_TYPE_STRING;
        bind[2].buffer = const_cast<char*>(sceneId.c_str());
        bind[2].buffer_length = scLen;
        bind[2].length = &scLen;
        mysql_stmt_bind_param(stmt, bind);

        if (mysql_stmt_execute(stmt) == 0)
            callback(mysql_stmt_insert_id(stmt));
        else
            callback(0);
        mysql_stmt_close(stmt);
    });
}

void SessionDAO::updateSessionScene(uint64_t sessionId,
                                     const std::string& sceneId,
                                     std::function<void(bool)> callback)
{
    dbPool_->submit([sessionId, sceneId, callback](std::shared_ptr<MYSQL> conn) {
        MYSQL_STMT* stmt = mysql_stmt_init(conn.get());
        const char* sql = "UPDATE sessions SET scene_id = ? WHERE id = ?";
        if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0)
        {
            mysql_stmt_close(stmt);
            callback(false);
            return;
        }

        MYSQL_BIND bind[2];
        memset(bind, 0, sizeof(bind));
        unsigned long scLen = sceneId.size();
        bind[0].buffer_type = MYSQL_TYPE_STRING;
        bind[0].buffer = const_cast<char*>(sceneId.c_str());
        bind[0].buffer_length = scLen;
        bind[0].length = &scLen;
        bind[1].buffer_type = MYSQL_TYPE_LONGLONG;
        uint64_t sid_copy = sessionId;
        bind[1].buffer = &sid_copy;
        mysql_stmt_bind_param(stmt, bind);

        bool ok = mysql_stmt_execute(stmt) == 0;
        mysql_stmt_close(stmt);
        callback(ok);
    });
}

void SessionDAO::endSession(uint64_t sessionId,
                             std::function<void(bool)> callback)
{
    dbPool_->submit([sessionId, callback](std::shared_ptr<MYSQL> conn) {
        MYSQL_STMT* stmt = mysql_stmt_init(conn.get());
        const char* sql = "UPDATE sessions SET status = 0 WHERE id = ?";
        if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0)
        {
            mysql_stmt_close(stmt);
            callback(false);
            return;
        }

        MYSQL_BIND bind[1];
        memset(bind, 0, sizeof(bind));
        bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
        uint64_t sid = sessionId;
        bind[0].buffer = &sid;
        mysql_stmt_bind_param(stmt, bind);

        bool ok = mysql_stmt_execute(stmt) == 0;
        mysql_stmt_close(stmt);
        callback(ok);
    });
}

void SessionDAO::findActiveSessionsByUser(uint64_t userId,
                                           std::function<void(const std::vector<Session>&)> callback)
{
    dbPool_->submit([userId, callback](std::shared_ptr<MYSQL> conn) {
        MYSQL_STMT* stmt = mysql_stmt_init(conn.get());
        const char* sql = "SELECT id, session_token, user_id, scene_id, status, "
                          "created_at, updated_at FROM sessions "
                          "WHERE user_id = ? AND status = 1";
        if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0)
        {
            mysql_stmt_close(stmt);
            callback(std::vector<Session>());
            return;
        }

        MYSQL_BIND bind[1];
        memset(bind, 0, sizeof(bind));
        bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
        uint64_t uid_copy = userId;
        bind[0].buffer = &uid_copy;
        mysql_stmt_bind_param(stmt, bind);
        mysql_stmt_execute(stmt);

        uint64_t id = 0, uid = 0;
        char sToken[256] = {0}, scene[128] = {0};
        int status = 0;
        char cAt[64] = {0}, uAt[64] = {0};
        MYSQL_BIND result[7];
        memset(result, 0, sizeof(result));
        result[0].buffer_type = MYSQL_TYPE_LONGLONG; result[0].buffer = &id;
        result[1].buffer_type = MYSQL_TYPE_STRING;   result[1].buffer = sToken;
        result[1].buffer_length = sizeof(sToken);
        result[2].buffer_type = MYSQL_TYPE_LONGLONG; result[2].buffer = &uid;
        result[3].buffer_type = MYSQL_TYPE_STRING;   result[3].buffer = scene;
        result[3].buffer_length = sizeof(scene);
        result[4].buffer_type = MYSQL_TYPE_LONG;     result[4].buffer = &status;
        result[5].buffer_type = MYSQL_TYPE_STRING;   result[5].buffer = cAt;
        result[5].buffer_length = sizeof(cAt);
        result[6].buffer_type = MYSQL_TYPE_STRING;   result[6].buffer = uAt;
        result[6].buffer_length = sizeof(uAt);
        mysql_stmt_bind_result(stmt, result);

        std::vector<Session> sessions;
        while (mysql_stmt_fetch(stmt) == 0)
        {
            Session s;
            s.id = id; s.sessionToken = sToken; s.userId = uid;
            s.sceneId = scene; s.status = status;
            s.createdAt = cAt; s.updatedAt = uAt;
            sessions.push_back(std::move(s));
        }
        mysql_stmt_close(stmt);
        callback(sessions);
    });
}
