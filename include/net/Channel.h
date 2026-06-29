#pragma once

#include <functional>
#include <memory>

#include "noncopyable.h"
#include "Timestamp.h"

class EventLoop;

/**
 * 理清楚 EventLoop、Channel、Poller之间的关系  Reactor模型上对应多路事件分发器
 * Channel理解为通道 封装了sockfd和其感兴趣的event 如EPOLLIN、EPOLLOUT事件 还绑定了poller返回的具体事件
 **/
class  Channel : noncopyable
{
public:
    using EventCallback = std::function<void()>; // muduo仍使用typedef
    using ReadEventCallback = std::function<void(Timestamp)>;

    Channel(EventLoop *loop, int fd);
    ~Channel();

    // fd得到Poller通知以后 处理事件 handleEvent在EventLoop::loop()中调用
    void handleEvent(Timestamp receiveTime);

    // 设置回调函数对象,提供“注册方法”（用来存回调函数）,这就是注册方法，执行这个方法的过程就是「注册」
    void setReadCallback(ReadEventCallback cb) { readCallback_ = std::move(cb); } //设置读事件的处理回调  把回调函数存到成员变量readCallback_里（注册的核心）
    void setWriteCallback(EventCallback cb) { writeCallback_ = std::move(cb); }
    void setCloseCallback(EventCallback cb) { closeCallback_ = std::move(cb); }
    void setErrorCallback(EventCallback cb) { errorCallback_ = std::move(cb); }

    // 防止当channel被手动remove掉 channel还在执行回调操作
    void tie(const std::shared_ptr<void> &);

    int fd() const { return fd_; }
    int events() const { return events_; }
    void set_revents(int revt) { revents_ = revt; }

    // 注册（设置）fd相应的事件状态 相当于epoll_ctl add delete
    void enableReading() { events_ |= kReadEvent; update(); } // 开启 FD 的读事件监听 events_ = events_ | kReadEvent;
    void disableReading() { events_ &= ~kReadEvent; update(); } // 关闭 FD 的读事件监听
    void enableWriting() { events_ |= kWriteEvent; update(); } // 开启 FD 的写事件监听
    void disableWriting() { events_ &= ~kWriteEvent; update(); }// 关闭 FD 的写事件监听
    void disableAll() { events_ = kNoneEvent; update(); }

    // 返回fd当前的事件状态
    bool isNoneEvent() const { return events_ == kNoneEvent; }
    bool isWriting() const { return events_ & kWriteEvent; }
    bool isReading() const { return events_ & kReadEvent; }

    int index() { return index_; }
    void set_index(int idx) { index_ = idx; }

    // one loop per thread
    EventLoop *ownerLoop() { return loop_; }
    void remove();
private:

    void update();
    void handleEventWithGuard(Timestamp receiveTime);

    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWriteEvent;

    EventLoop *loop_; // 事件循环
    const int fd_;    // fd，Poller监听的对象
    int events_;      // 注册fd感兴趣的事件
    int revents_;     // Poller返回的具体发生的事件
    int index_;       //某个channel是否添加至Poller

    std::weak_ptr<void> tie_; //弱引用，不增加上层对象的引用计数（不影响对象的销毁），仅用于「观察」对象是否存活
    bool tied_; //标记 Channel 是否已经绑定了上层对象

    // 因为channel通道里可获知fd最终发生的具体的事件events，所以它负责调用具体事件的回调操作
    ReadEventCallback readCallback_;  //是std::function<void(Timestamp)> 模版类的实例
    EventCallback writeCallback_; //std::function<void()> 的实例
    EventCallback closeCallback_; //std::function<void()> 的实例
    EventCallback errorCallback_; //std::function<void()> 的实例
};