#pragma once

#include <string>
#include <sys/stat.h>
#include "LFU.h"

/**
 * @brief 静态文件缓存条目
 *
 * 存储文件内容、MIME类型及最后修改时间，
 * 作为 KHashLfuCache 的 Value 类型。
 */
struct CachedFileEntry
{
    std::string content;       // 文件内容
    std::string contentType;   // MIME 类型 (如 "text/html")
    time_t lastModified;       // 文件最后修改时间(用于缓存失效判断)
    size_t fileSize;           // 文件大小(字节)

    CachedFileEntry()
        : lastModified(0), fileSize(0) {}

    CachedFileEntry(std::string c, std::string mime, time_t mtime, size_t size)
        : content(std::move(c)),
          contentType(std::move(mime)),
          lastModified(mtime),
          fileSize(size) {}
};

/**
 * @brief 静态文件缓存 (基于哈希分片 LFU)
 *
 * 对热点静态文件进行内容级缓存，避免高并发下重复磁盘 I/O。
 * 底层使用 KHashLfuCache，具备：
 *   - 哈希分片减少多线程锁竞争
 *   - LFU 淘汰策略 (优先淘汰访问频率最低的文件)
 *   - 频率老化机制 (防止旧热点永久驻留)
 *
 * 典型用法:
 *   StaticFileCache cache(200);                   // 最多缓存200个文件
 *   CachedFileEntry entry;
 *   if (cache.get("/www/index.html", entry)) {    // 命中
 *       // 使用 entry.content, entry.contentType
 *   } else {                                      // 未命中
 *       // 从磁盘读取，写入缓存
 *       cache.put("/www/index.html", CachedFileEntry(...));
 *   }
 */
class StaticFileCache
{
public:
    /**
     * @param capacity  最大缓存文件数 (默认200)
     * @param sliceNum  哈希分片数 (默认0=自动取CPU核心数)
     */
    explicit StaticFileCache(size_t capacity = 200, int sliceNum = 0)
        : cache_(capacity, sliceNum)
    {}

    /**
     * @brief 查找缓存
     * @return true=命中, false=未命中
     */
    bool get(const std::string& filePath, CachedFileEntry& entry)
    {
        return cache_.get(filePath, entry);
    }

    /**
     * @brief 添加/更新缓存条目
     */
    void put(const std::string& filePath, const CachedFileEntry& entry)
    {
        cache_.put(filePath, entry);
    }

    /// 清空所有缓存
    void purge() { cache_.purge(); }

    /**
     * @brief 根据文件扩展名返回 MIME 类型
     *
     * 覆盖常见的 Web 静态资源类型。
     * 未识别的扩展名返回 "application/octet-stream"。
     */
    static std::string getMimeType(const std::string& path)
    {
        // 提取扩展名
        auto dotPos = path.rfind('.');
        if (dotPos == std::string::npos)
            return "application/octet-stream";

        std::string ext = path.substr(dotPos);

        if (ext == ".html" || ext == ".htm")   return "text/html; charset=utf-8";
        if (ext == ".css")                      return "text/css; charset=utf-8";
        if (ext == ".js")                       return "application/javascript; charset=utf-8";
        if (ext == ".json")                     return "application/json; charset=utf-8";
        if (ext == ".xml")                      return "application/xml; charset=utf-8";
        if (ext == ".txt")                      return "text/plain; charset=utf-8";
        if (ext == ".svg")                      return "image/svg+xml";
        if (ext == ".png")                      return "image/png";
        if (ext == ".jpg" || ext == ".jpeg")    return "image/jpeg";
        if (ext == ".gif")                      return "image/gif";
        if (ext == ".ico")                      return "image/x-icon";
        if (ext == ".woff")                     return "font/woff";
        if (ext == ".woff2")                    return "font/woff2";
        if (ext == ".ttf")                      return "font/ttf";
        if (ext == ".wasm")                     return "application/wasm";
        if (ext == ".pdf")                      return "application/pdf";
        if (ext == ".mp4")                      return "video/mp4";
        if (ext == ".webp")                     return "image/webp";

        return "application/octet-stream";
    }

private:
    cache::KHashLfuCache<std::string, CachedFileEntry> cache_;
};
