#include <http/HttpContext.h>
#include <Logger.h>
#include <string.h>
#include <algorithm>

// ========== 工具函数 ==========

const char* HttpContext::findCRLF(const char* begin, const char* end)
{
    for (const char* p = begin; p + 1 < end; ++p)
    {
        if (*p == '\r' && *(p + 1) == '\n')
        {
            return p; // 返回'\r'的位置
        }
    }
    return nullptr;
}

HttpRequest::Method HttpContext::stringToMethod(const std::string& m)
{
    if (m == "GET")     return HttpRequest::kGet;
    if (m == "POST")    return HttpRequest::kPost;
    if (m == "HEAD")    return HttpRequest::kHead;
    if (m == "PUT")     return HttpRequest::kPut;
    if (m == "DELETE")  return HttpRequest::kDelete;
    return HttpRequest::kInvalid;
}

// ========== 解析入口 ==========

bool HttpContext::parseRequest(Buffer* buf, Timestamp receiveTime)
{
    // 仅在首次进入时设置接收时间
    if (state_ == kExpectRequestLine)
    {
        request_.setReceiveTime(receiveTime);
    }

    while (state_ != kGotCompleteRequest)
    {
        switch (state_)
        {
        case kExpectRequestLine:
        {
            const char* begin = buf->peek();
            const char* end   = begin + buf->readableBytes();
            const char* crlf  = findCRLF(begin, end);

            if (!crlf)
            {
                // 还没有收到完整的请求行，等待更多数据
                return true;
            }

            if (!processRequestLine(begin, crlf))
            {
                return false; // 请求行格式错误 → 400
            }

            // 消费请求行(包含\r\n)
            buf->retrieve(crlf - begin + 2);
            state_ = kExpectHeaders;
            break;
        }

        case kExpectHeaders:
        {
            if (!parseHeaders(buf))
            {
                return false; // 头部格式错误 → 400
            }
            // parseHeaders() 内部会更新 state_
            break;
        }

        case kExpectBody:
        {
            if (!parseBody(buf))
            {
                return false;
            }
            // parseBody() 内部会更新 state_
            break;
        }

        case kGotCompleteRequest:
            break; // 不应该到达这里
        }
    }

    return true;
}

// ========== 请求行解析 ==========

bool HttpContext::processRequestLine(const char* begin, const char* end)
{
    // 提取三个令牌: METHOD SP PATH?QUERY SP VERSION
    // 例如: "GET /index.html?key=val HTTP/1.1"

    const char* start = begin;
    const char* space1 = nullptr;
    const char* space2 = nullptr;

    for (const char* p = begin; p < end; ++p)
    {
        if (*p == ' ')
        {
            if (!space1)
            {
                space1 = p;
            }
            else if (!space2)
            {
                space2 = p;
                break; // 只需要前两个空格
            }
        }
    }

    if (!space1 || !space2)
    {
        return false; // 格式错误: "METHOD URL VERSION" 至少需要两个空格
    }

    // 1. 方法
    std::string methodStr(start, space1 - start);
    HttpRequest::Method method = stringToMethod(methodStr);
    if (method == HttpRequest::kInvalid)
    {
        LOG_WARN << "Unknown HTTP method: " << methodStr;
        return false;
    }
    request_.setMethod(method);

    // 2. URL (path + optional query)
    std::string url(space1 + 1, space2 - space1 - 1);
    size_t questionPos = url.find('?');
    if (questionPos != std::string::npos)
    {
        request_.setPath(url.substr(0, questionPos));
        request_.setQuery(url.substr(questionPos + 1));
    }
    else
    {
        request_.setPath(url);
        // query保持空字符串
    }

    // 3. HTTP版本
    request_.setVersion(std::string(space2 + 1, end - space2 - 1));

    return true;
}

// ========== 头部解析 ==========

bool HttpContext::parseHeaders(Buffer* buf)
{
    while (true)
    {
        const char* begin = buf->peek();
        const char* end   = begin + buf->readableBytes();
        const char* crlf  = findCRLF(begin, end);

        if (!crlf)
        {
            // 头部还没收完，等待更多数据
            // state_ 保持 kExpectHeaders
            return true;
        }

        // 检查是否为 空行(仅\r\n)，表示头部结束
        if (crlf == begin)
        {
            // 消费空行
            buf->retrieve(2);

            // 判断是否需要读body
            const std::string& lenStr = request_.getHeader("content-length");
            if (!lenStr.empty())
            {
                contentLength_ = static_cast<size_t>(std::stoul(lenStr));
                if (contentLength_ > 0)
                {
                    state_ = kExpectBody;
                }
                else
                {
                    state_ = kGotCompleteRequest;
                }
            }
            else
            {
                // GET / HEAD / DELETE 等通常没有 body
                state_ = kGotCompleteRequest;
            }
            return true;
        }

        // 解析一行header: "Key: Value\r\n"
        const char* colon = nullptr;
        for (const char* p = begin; p < crlf; ++p)
        {
            if (*p == ':')
            {
                colon = p;
                break;
            }
        }

        if (!colon)
        {
            // 非法header行(没有冒号)
            LOG_WARN << "Invalid header line (no colon)";
            return false;
        }

        std::string key(begin, colon - begin);
        // 跳过冒号后面的空格
        const char* valueStart = colon + 1;
        while (valueStart < crlf && *valueStart == ' ')
        {
            ++valueStart;
        }
        std::string value(valueStart, crlf - valueStart);

        request_.addHeader(key, value);

        // 消费这一行
        buf->retrieve(crlf - begin + 2);
    }
}

// ========== Body解析 ==========

bool HttpContext::parseBody(Buffer* buf)
{
    if (buf->readableBytes() < contentLength_)
    {
        // body还没收全, 等待更多数据
        return true;
    }

    // 读取body
    request_.setBody(std::string(buf->peek(), contentLength_));
    buf->retrieve(contentLength_);
    state_ = kGotCompleteRequest;
    return true;
}

// ========== 重置 ==========

void HttpContext::reset()
{
    HttpRequest dummy;
    request_.swap(dummy);
    state_          = kExpectRequestLine;
    contentLength_  = 0;
}
