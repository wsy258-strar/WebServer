#pragma once

#include <string>
#include <map>
#include <algorithm>
#include <cctype>

#include <base/Timestamp.h>

/**
 * @brief HTTP请求数据结构，存放HttpContext解析完成的请求信息
 *
 * 这是一个纯数据载体(POJO)，解析由HttpContext完成，
 * HttpServer在完整请求到达后调用用户路由回调。
 */
class HttpRequest
{
public:
    enum Method
    {
        kInvalid,
        kGet,
        kPost,
        kHead,
        kPut,
        kDelete
    };

    HttpRequest()
        : method_(kInvalid),
          queryParsed_(false)
    {}

    // ---------- setters (由HttpContext在解析时调用) ----------
    void setMethod(Method m) { method_ = m; }
    void setPath(const std::string& path) { path_ = path; }
    void setQuery(const std::string& query) { query_ = query; }
    void setVersion(const std::string& version) { version_ = version; }
    void setReceiveTime(Timestamp t) { receiveTime_ = t; }
    void setBody(const std::string& body) { body_ = body; }

    void addHeader(const std::string& key, const std::string& value)
    {
        // HTTP header名称不区分大小写，统一转为小写存储
        std::string lowerKey = key;
        std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);
        headers_[lowerKey] = value;
    }

    // ---------- accessors ----------
    Method method() const { return method_; }
    const std::string& path() const { return path_; }
    const std::string& query() const { return query_; }
    const std::string& version() const { return version_; }
    const std::string& body() const { return body_; }
    Timestamp receiveTime() const { return receiveTime_; }

    /// 查询某个请求头，不存在返回空字符串
    std::string getHeader(const std::string& field) const
    {
        std::string lowerField = field;
        std::transform(lowerField.begin(), lowerField.end(), lowerField.begin(), ::tolower);
        auto it = headers_.find(lowerField);
        return it != headers_.end() ? it->second : std::string();
    }

    const std::map<std::string, std::string>& headers() const { return headers_; }

    // ---------- helpers ----------

    /// 为 keep-alive 复用高效重置请求对象
    void swap(HttpRequest& other)
    {
        std::swap(method_, other.method_);
        path_.swap(other.path_);
        query_.swap(other.query_);
        version_.swap(other.version_);
        headers_.swap(other.headers_);
        body_.swap(other.body_);
        std::swap(receiveTime_, other.receiveTime_);
        std::swap(queryParsed_, other.queryParsed_);
        queryParams_.swap(other.queryParams_);
    }

    /// 懒解析 query string (e.g. "key1=val1&key2=val2")
    const std::map<std::string, std::string>& queryParameters() const;

    /// 将Method枚举转为字符串
    static const char* methodString(Method m)
    {
        switch (m)
        {
        case kGet:    return "GET";
        case kPost:   return "POST";
        case kHead:   return "HEAD";
        case kPut:    return "PUT";
        case kDelete: return "DELETE";
        default:      return "UNKNOWN";
        }
    }

private:
    Method method_;
    std::string path_;       // 不含query部分，如 "/index.html"
    std::string query_;      // '?' 之后的原始字符串，如 "key=val&x=y"
    std::string version_;    // 如 "HTTP/1.1"、"HTTP/1.0"
    std::map<std::string, std::string> headers_;
    std::string body_;
    Timestamp receiveTime_;

    // 懒缓存：第一次调用 queryParameters() 时解析，后续直接返回
    mutable bool queryParsed_;
    mutable std::map<std::string, std::string> queryParams_;
};

/// 简单的URL解码(支持%XX，解码后的字符串长度≤原始长度)
std::string urlDecode(const std::string& str);
