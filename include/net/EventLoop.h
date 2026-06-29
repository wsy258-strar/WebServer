#pragma once

#include <functional>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>

#include "noncopyable.h"
#include "Timestamp.h"
#include "CurrentThread.h"
#include "TimerQueue.h"
class Channel;
class Poller;

// 事件循环类 主要包含了两个大模块 Channel Poller(epoll的抽象)
class EventLoop : noncopyable //不可拷贝
{
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    // 开启事件循环
    void loop();
    // 退出事件循环
    void quit();

    Timestamp pollReturnTime() const { return pollRetureTime_; }

    // 在当前loop中执行
    void runInLoop(Functor cb);
    // 把上层注册的回调函数cb放入队列中 唤醒loop所在的线程执行cb
    void queueInLoop(Functor cb);

    // 通过eventfd唤醒loop所在的线程
    void wakeup();

    // EventLoop的方法 => Poller的方法
    void updateChannel(Channel *channel);
    void removeChannel(Channel *channel);
    bool hasChannel(Channel *channel);

    // 判断EventLoop对象是否在自己的线程里
    bool isInLoopThread() const { return threadId_ == CurrentThread::tid(); } // threadId_为EventLoop创建时的线程id CurrentThread::tid()为当前线程id
    /**
     * 定时任务相关函数
     */
    // 指定绝对时间点执行一次性定时任务
    void runAt(Timestamp timestamp, Functor &&cb) 
    {
        //Timestamp timestamp：定时任务的绝对执行时间点;Functor &&cb：待执行的回调函数;
        // std::move(cb)：将右值引用的回调对象所有权转移给底层 TimerQueue;
        //  0.0：传递给 TimerQueue::addTimer 的「周期值」，0.0 明确表示该任务是一次性的
        timerQueue_->addTimer(std::move(cb), timestamp, 0.0);
    }

    //延时相对时间执行一次性定时任务
    void runAfter(double waitTime, Functor &&cb)
    {
        // 1. 计算绝对执行时间：当前时间 + 等待时间
        Timestamp time(addTime(Timestamp::now(), waitTime));
        runAt(time, std::move(cb));
    }

    //周期性执行 ——固定间隔循环执行定时任务
    void runEvery(double interval, Functor &&cb)
    {
        // 1. 计算首次执行的绝对时间：当前时间 + 周期间隔
        Timestamp timestamp(addTime(Timestamp::now(), interval));
        // 2. 直接调用TimerQueue，第三个参数传周期值表示周期性任务
        timerQueue_->addTimer(std::move(cb), timestamp, interval);
    }

private:
    void handleRead();        // 给eventfd返回的文件描述符wakeupFd_绑定的事件回调 当wakeup()时 即有事件发生时 调用handleRead()读wakeupFd_的8字节 同时唤醒阻塞的epoll_wait
    void doPendingFunctors(); // 执行上层回调

    using ChannelList = std::vector<Channel *>;

    std::atomic_bool looping_; // 原子布尔变量 原子操作 底层通过CAS实现
    std::atomic_bool quit_;    // 标识退出loop循环

    const pid_t threadId_; // 记录当前EventLoop是被哪个线程id创建的 即标识了当前EventLoop的所属线程id

    Timestamp pollRetureTime_; // Poller返回发生事件的Channels的时间点
    std::unique_ptr<Poller> poller_; //事件检测器,IO 多路复用的抽象封装，负责检测所有注册的 FD 上的活跃 IO 事件
    std::unique_ptr<TimerQueue> timerQueue_; //实现定时器功能的核心载体与唯一实现依赖
    int wakeupFd_; // 是 EventLoop 中用于唤醒阻塞事件循环的专用文件描述符（FD）;作用：当mainLoop获取一个新用户的Channel 需通过轮询算法选择一个subLoop 通过该成员唤醒subLoop处理Channel
    std::unique_ptr<Channel> wakeupChannel_; //是 EventLoop 中封装「wakeup 读 FD」的 Channel，核心作用是唤醒阻塞在 Poller::poll () 的 EventLoop；

    ChannelList activeChannels_; // 返回Poller检测到当前有事件发生的所有Channel列表

    std::atomic_bool callingPendingFunctors_; // 标识当前loop是否有需要执行的回调操作
    std::vector<Functor> pendingFunctors_;    // 存储loop需要执行的所有回调操作
    std::mutex mutex_;                        // 互斥锁 用来保护上面vector容器的线程安全操作
};