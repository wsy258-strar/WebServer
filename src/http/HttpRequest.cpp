#include <http/HttpRequest.h>
#include <sstream>
#include <iomanip>

/// 简单的十六进制字符转数值，失败返回 -1
static int hexDigitValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

std::string urlDecode(const std::string& str)
{
    std::string result;
    result.reserve(str.size());

    for (size_t i = 0; i < str.size(); ++i)
    {
        if (str[i] == '%' && i + 2 < str.size())
        {
            int high = hexDigitValue(str[i + 1]);
            int low  = hexDigitValue(str[i + 2]);
            if (high >= 0 && low >= 0)
            {
                result.push_back(static_cast<char>(high * 16 + low));
                i += 2;
                continue;
            }
        }
        if (str[i] == '+')
        {
            result.push_back(' ');
            continue;
        }
        result.push_back(str[i]);
    }
    return result;
}

const std::map<std::string, std::string>& HttpRequest::queryParameters() const
{
    if (queryParsed_)
    {
        return queryParams_;
    }

    queryParsed_ = true;
    const std::string& q = query_;
    if (q.empty()) return queryParams_;

    std::istringstream stream(q);
    std::string pair;
    while (std::getline(stream, pair, '&'))
    {
        if (pair.empty()) continue;

        size_t eqPos = pair.find('=');
        if (eqPos != std::string::npos)
        {
            std::string key   = urlDecode(pair.substr(0, eqPos));
            std::string value = urlDecode(pair.substr(eqPos + 1));
            queryParams_[key] = value;
        }
        else
        {
            queryParams_[urlDecode(pair)] = "";
        }
    }
    return queryParams_;
}
