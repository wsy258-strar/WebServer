#pragma once

#include <string>
#include <map>

#include <net/Buffer.h>
#include <base/noncopyable.h>

/**
 * @brief HTTP响应构建器
 *
 * 用户路由回调通过此对象设置状态码、Content-Type、Body等，
 * HttpServer负责调用appendToBuffer()将其序列化后发送。
 */
class HttpResponse
{
public:
    enum HttpStatusCode
    {
        k200Ok                  = 200,
        k301MovedPermanently    = 301,
        k400BadRequest          = 400,
        k404NotFound            = 404,
        k500InternalServerError = 500,
    };

    explicit HttpResponse(bool close)
        : statusCode_(k200Ok),
          closeConnection_(close)
    {}

    // ---------- setters (用户路由回调使用) ----------
    void setStatusCode(HttpStatusCode code) { statusCode_ = code; }
    void setStatusMessage(const std::string& message) { statusMessage_ = message; }
    void setCloseConnection(bool on) { closeConnection_ = on; }
    bool closeConnection() const { return closeConnection_; }

    void setContentType(const std::string& contentType)
    { addHeader("Content-Type", contentType); }

    void setBody(const std::string& body) { body_ = body; }

    void addHeader(const std::string& key, const std::string& value)
    { headers_[key] = value; }

    // ---------- 序列化 ----------
    /// 将完整的HTTP响应(状态行+头部+空行+body)追加到output
    void appendToBuffer(Buffer* output) const;

    // ---------- 静态工厂 ----------
    /// 构建一个包含简单HTML错误页面的Response
    static HttpResponse makeErrorResponse(HttpStatusCode code, bool close,
                                          const std::string& extraInfo = "");

private:
    /// 根据状态码返回默认的状态消息字符串
    static const char* statusMessage(int code);

    HttpStatusCode statusCode_;
    std::string statusMessage_;
    std::map<std::string, std::string> headers_;
    std::string body_;
    bool closeConnection_;
};
