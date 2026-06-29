#include <EventLoopThread.h>
#include <EventLoop.h>

EventLoopThread::EventLoopThread(const ThreadInitCallback &cb,
                                 const std::string &name)
    : loop_(nullptr)
    , exiting_(false)
    , thread_(std::bind(&EventLoopThread::threadFunc, this), name)
    , mutex_()
    , cond_()
    , callback_(cb)
{
}

EventLoopThread::~EventLoopThread()
{
    exiting_ = true;
    if (loop_ != nullptr)
    {
        loop_->quit();
        thread_.join();
    }
}
//创建并启动 IO 线程、返回该线程绑定的 EventLoop 对象
EventLoop *EventLoopThread::startLoop()
{
    thread_.start(); // 启用底层线程Thread类对象thread_中通过start()创建的线程

    EventLoop *loop = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        //条件变量等待：直到 IO 线程初始化完 loop_（loop_ != nullptr）
        cond_.wait(lock, [this](){return loop_ != nullptr;});
        loop = loop_;
    }
    return loop;
}

// 下面这个方法 是在单独的新线程里运行的
// IO 线程的真正入口函数,是 IO 线程的「初始化 + 运行 + 收尾」全流程 —— 先造好「事件循环引擎（EventLoop）」，告诉主线程「引擎已就绪」，然后启动引擎持续处理 IO 事件，最后引擎停了就清理标记
void EventLoopThread::threadFunc()
{
    EventLoop loop; // 创建一个独立的EventLoop对象 和上面的线程是一一对应的 即one loop per thread

    if (callback_)
    {
        callback_(&loop);
    }
    //同步主线程：告知loop_已初始化完成，唤醒startLoop()中的等待
    {
        std::unique_lock<std::mutex> lock(mutex_);  //保护 loop_ 的赋值（防止主线程同时读 loop_ 导致竞态）
        loop_ = &loop;
        cond_.notify_one(); // 唤醒主线程的cond_.wait()，告诉主线程「loop_ 已有效，可以返回了」
    }
    //核心：启动EventLoop的事件循环（Reactor主循环，阻塞直到quit()）
    loop.loop();    // 执行EventLoop的loop() 开启了底层的Poller的poll()   1.阻塞在 Poller::poll()（epoll_wait）等待 IO 事件；
                                                                        // 2.处理就绪的 IO 事件（socket 读写、timerfd 定时、eventfd 唤醒）；
                                                                        //3.执行跨线程提交的任务；
    //收尾：循环退出后，重置loop_为nullptr（标记EventLoop已失效）
    std::unique_lock<std::mutex> lock(mutex_);
    loop_ = nullptr;
}