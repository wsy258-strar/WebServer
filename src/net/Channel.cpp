#include <sys/epoll.h>

#include <Channel.h>
#include <EventLoop.h>
#include <Logger.h>

const int Channel::kNoneEvent = 0; //空事件
const int Channel::kReadEvent = EPOLLIN | EPOLLPRI; //读事件
const int Channel::kWriteEvent = EPOLLOUT; //写事件

// EventLoop: ChannelList Poller
Channel::Channel(EventLoop *loop, int fd)
    : loop_(loop)
    , fd_(fd)
    , events_(0)
    , revents_(0)
    , index_(-1)
    , tied_(false)
{
}

Channel::~Channel()
{
}

// channel的tie方法什么时候调用过?  TcpConnection => channel
/**
 * TcpConnection中注册了Channel对应的回调函数，传入的回调函数均为TcpConnection
 * 对象的成员方法，因此可以说明一点就是：Channel的结束一定晚于TcpConnection对象！
 * 此处用tie去解决TcpConnection和Channel的生命周期时长问题，从而保证了Channel对象能够在
 * TcpConnection销毁前销毁。

 * Channel 作为 FD 的通用封装，其回调（读 / 写 / 关闭 / 错误）是异步触发的：当 Poller 检测到 FD 事件并调用 Channel 的 handleEvent() 时，
 * 对应的「上层对象」（比如 TcpConnection）可能已经被销毁（比如客户端断开连接，TcpConnection 被析构）。如果此时回调中访问该上层对象（比如 
 * TcpConnection 的成员），就会触发野指针访问，导致程序崩溃。
 * muduo 解决这个问题的核心思路是：让 Channel 弱引用上层对象，回调执行前先检查对象是否存活；若存活则临时延长其生命周期，保证回调执行期间对象
 * 不被销毁——tie_ 和 tied_ 就是实现这个逻辑的核心变量。
 **/
void Channel::tie(const std::shared_ptr<void> &obj)
{
    tie_ = obj;
    tied_ = true;
}
//update 和remove => EpollPoller 更新channel在poller中的状态
/**
 * 当改变channel所表示的fd的events事件后，update负责再poller里面更改fd相应的事件epoll_ctl
 **/
void Channel::update()
{
    // 通过channel所属的eventloop，调用poller的相应方法，注册fd的events事件
    loop_->updateChannel(this);
}

// 在channel所属的EventLoop中把当前的channel删除掉
void Channel::remove()
{
    loop_->removeChannel(this);
}

//先检查关联对象（如 TcpConnection）是否存活，再执行实际事件分发
void Channel::handleEvent(Timestamp receiveTime)
{
    if (tied_)
    {   //.lock() 将「弱引用（tie_）」升级为「强引用（shared_ptr）」
        std::shared_ptr<void> guard = tie_.lock();  // 若目标对象存活：返回指向该对象的 shared_ptr（引用计数 + 1，强引用持有）；
                                                    // 若目标对象已销毁：返回空 shared_ptr
        if (guard)
        {
            handleEventWithGuard(receiveTime);
        }
        // 如果提升失败了 就不做任何处理 说明Channel的TcpConnection对象已经不存在了
    }
    else
    {
        handleEventWithGuard(receiveTime);
    }
}

//WithGuard 表示执行时已保证线程安全，比如在 EventLoop 绑定线程中执行
void Channel::handleEventWithGuard(Timestamp receiveTime)
{
    LOG_INFO<<"channel handleEvent revents:"<<revents_;
    // 关闭
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) // 当TcpConnection对应Channel 通过触发EPOLLHUP且无EPOLLIN → 对应TCP连接被动关闭（如对端shutdown写端）
    {
        if (closeCallback_)
        {
            closeCallback_();
        }
    }
    // 错误
    if (revents_ & EPOLLERR)
    {
        if (errorCallback_)   //将回调函数封装为errorCallback_对象
        {
            errorCallback_(); //调用回调函数
        }
    }
    // 读
    if (revents_ & (EPOLLIN | EPOLLPRI))
    {
        if (readCallback_)
        {
            readCallback_(receiveTime);
        }
    }
    // 写
    if (revents_ & EPOLLOUT)
    {
        if (writeCallback_)
        {
            writeCallback_();
        }
    }
}