#pragma once

#include <http/HttpRequest.h>
#include <net/Buffer.h>
#include <base/Timestamp.h>
#include <base/noncopyable.h>

/**
 * @brief HTTP请求增量解析器（状态机）
 *
 * 直接在Buffer上操作(peek/retrieve)，零拷贝。
 * 处理TCP流式数据的特性：一次onMessage可能只收到部分请求，
 * parseRequest()返回true表示解析正常(但可能还需更多数据)，
 * 返回false表示请求格式错误(应返回400)。
 *
 * 状态迁移:
 *   kExpectRequestLine → kExpectHeaders → kExpectBody → kGotCompleteRequest
 *                                                              │
 *                                                     reset()  │
 *                                                        ◄─────┘
 */
class HttpContext : noncopyable
{
public:
    enum ParseState
    {
        kExpectRequestLine,
        kExpectHeaders,
        kExpectBody,
        kGotCompleteRequest,
    };

    HttpContext()
        : state_(kExpectRequestLine),
          contentLength_(0)
    {}

    /**
     * @brief 从Buffer中增量解析HTTP请求
     * @return false 请求格式错误(应返回400)
     * @return true  解析正常，调用者需再检查gotAll()判断是否完成
     */
    bool parseRequest(Buffer* buf, Timestamp receiveTime);

    /// 是否已解析出一个完整请求
    bool gotAll() const { return state_ == kGotCompleteRequest; }

    /// 重置解析器状态，用于keep-alive的下一个请求
    void reset();

    const HttpRequest& request() const { return request_; }
    HttpRequest& request() { return request_; }

private:
    bool processRequestLine(const char* begin, const char* end);
    bool parseHeaders(Buffer* buf);
    bool parseBody(Buffer* buf);

    /// 在[begin, end)范围内查找 "\r\n"，返回'\r'的指针，未找到返回nullptr
    static const char* findCRLF(const char* begin, const char* end);
    static bool isSpace(char c) { return c == ' ' || c == '\t'; }
    static HttpRequest::Method stringToMethod(const std::string& m);

    ParseState state_;
    HttpRequest request_;
    size_t contentLength_;  // 从Content-Length头解析，GET/HEAD请求为0
};
