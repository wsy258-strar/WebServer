#include "AsyncLogging.h"
#include <stdio.h>
AsyncLogging::AsyncLogging(const std::string &basename, off_t rollSize, int flushInterval)
    :
      flushInterval_(flushInterval),
      running_(false),
      basename_(basename),
      rollSize_(rollSize),
      thread_(std::bind(&AsyncLogging::threadFunc, this), "Logging"),
      mutex_(),
      cond_(),
      currentBuffer_(new LargeBuffer),
      nextBuffer_(new LargeBuffer),
      buffers_()
{
    currentBuffer_->bzero();
    nextBuffer_->bzero();
    buffers_.reserve(16); // 只维持队列长度2~16.
}
//前端
// 调用此函数解决前端把LOG_XXX<<"..."传递给后端，后端再将日志消息写入日志文件
void AsyncLogging::append(const char *logline, int len)
{
    std::lock_guard<std::mutex> lg(mutex_);
    // 缓冲区剩余的空间足够写入
    if (currentBuffer_->avail() > static_cast<size_t>(len))
    {
        currentBuffer_->append(logline, len);
    }
    else
    {
        buffers_.push_back(std::move(currentBuffer_));

        if (nextBuffer_)
        {
            currentBuffer_ = std::move(nextBuffer_);
        }
        else
        {
            currentBuffer_.reset(new LargeBuffer);
        }
        currentBuffer_->append(logline, len);
        // 唤醒后端线程写入磁盘
        cond_.notify_one();
    }
}

//后端
void AsyncLogging::threadFunc()
{
    // output写入磁盘接口
    LogFile output(basename_, rollSize_);
    BufferPtr newbuffer1(new LargeBuffer); // 生成newbuffer1替换currentbuffer_（前端）
    BufferPtr newbuffer2(new LargeBuffer); // 生成newbuffer2替换nextBuffer_（前端），其目的是为了防止后端缓冲区全满前端无法写入
    newbuffer1->bzero();
    newbuffer2->bzero();
    // 缓冲区数组置为16个，用于和前端缓冲区数组进行交换
    BufferVector buffersToWrite;
    buffersToWrite.reserve(16);
    while (running_)
    {
        {
            // 互斥锁保护这样就保证了其他前端线程无法向前端buffer写入数据
            std::unique_lock<std::mutex> lg(mutex_);
            if (buffers_.empty())
            {
                cond_.wait_for(lg, std::chrono::seconds(3)); //要么被前端 notify_one() 唤醒（日志满了），要么 3 秒超时唤醒（少量日志）
            }
            buffers_.push_back(std::move(currentBuffer_));
            currentBuffer_ = std::move(newbuffer1); // 生成newbuffer1替换currentbuffer_
            if (!nextBuffer_)
            {
                nextBuffer_ = std::move(newbuffer2);
            }
            buffersToWrite.swap(buffers_);//与前端缓冲区数组buffers_进行交换，将buffers_数据转移到buffersToWrite，buffers_数组为空
        }
        // 从待写缓冲区取出数据通过LogFile提供的接口写入到磁盘中
        for (auto &buffer : buffersToWrite)
        {
            output.append(buffer->data(), buffer->length());
        }
        // 优化：若缓冲区数量>2，仅保留最后2个（用于复用为newbuffer1/newbuffer2），其余释放
        if (buffersToWrite.size() > 2)
        {
            buffersToWrite.resize(2);
        }
        // 复用空缓冲区为newbuffer1（补充前端currentBuffer_的备用），避免频繁 new LargeBuffer（内存分配是慢操作）
        if (!newbuffer1)
        {
            newbuffer1 = std::move(buffersToWrite.back());// 取最后一个空缓冲区
            buffersToWrite.pop_back();
            newbuffer1->reset();// 清空缓冲区内容（复用内存，不是销毁）
        }
        // 复用空缓冲区为newbuffer2（补充前端nextBuffer_的备用）
        if (!newbuffer2)
        {
            newbuffer2 = std::move(buffersToWrite.back());
            buffersToWrite.pop_back();
            newbuffer2->reset();
        }
        buffersToWrite.clear(); // 清空后端缓冲队列
        output.flush();         // 清空文件夹缓冲区
    }
    output.flush(); // 确保一定清空。
}