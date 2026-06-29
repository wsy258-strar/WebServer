#pragma once

#include "../base/noncopyable.h"
#include "../base/Thread.h"
#include "FixedBuffer.h"
#include "LogStream.h"
#include "LogFile.h"

#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>

//从前端获得的 Buffer A 放⼊ 后端的 Buffer B中，并且将 Buffer B 的内容最终写⼊到磁盘中（也就是整个后端所作的内容）  
class AsyncLogging
{
public:
    AsyncLogging(const std::string &basename, off_t rollSize, int flushInterval=3);
    ~AsyncLogging()
    {
        if (running_)
        {
            stop();
        }
    }
    // 前端调用append写入日志
    void append(const char *logline, int len);
    void start()
    {
        running_ = true;
        thread_.start();
    }
    void stop()
    {
        running_ = false;
        cond_.notify_one();
    }

private:
    using LargeBuffer = FixedBuffer<kLargeBufferSize>;
    using BufferVector = std::vector<std::unique_ptr<LargeBuffer>>;
    // BufferVector::value_type 是 std::vector<std::unique_ptr<LargeBuffer>> 的元素类型，也就是 std::unique_ptr<LargeBuffer>。
    using BufferPtr = BufferVector::value_type;
    void threadFunc();
    const int flushInterval_; // 日志刷新时间
    std::atomic<bool> running_;
    const std::string basename_;
    const off_t rollSize_;
    Thread thread_;
    std::mutex mutex_;
    std::condition_variable cond_;
    
    BufferPtr currentBuffer_; //当前活跃缓冲区，前端业务线程写入日志的 “主缓冲区”，所有日志首先往这个缓冲区写
    BufferPtr nextBuffer_;//预分配的备用缓冲区，用于 currentBuffer_ 写满时快速替换，避免前端频繁创建新缓冲区（new 操作有开销），极致降低日志写入延迟
    BufferVector buffers_;
};