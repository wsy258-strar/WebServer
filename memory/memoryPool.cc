#include "memoryPool.h"

namespace memoryPool 
{
MemoryPool::MemoryPool(size_t BlockSize)
    : BlockSize_ (BlockSize)
{}

MemoryPool::~MemoryPool()
{
    // 把连续的block删除
    Slot* cur = firstBlock_;
    while (cur)
    {
        Slot* next = cur->next;
        // 等同于 free(reinterpret_cast<void*>(firstBlock_));
        // 转化为 void 指针，因为 void 类型不需要调用析构函数，只释放空间
        operator delete(reinterpret_cast<void*>(cur));
        cur = next;
    }
}

void MemoryPool::init(size_t size)
{
    assert(size > 0);
    SlotSize_ = size;
    firstBlock_ = nullptr;
    curSlot_ = nullptr;
    freeList_ = nullptr;
    lastSlot_ = nullptr;
}

void* MemoryPool::allocate()
{
    // 优先使用空闲链表中的内存槽
    if (freeList_ != nullptr)
    {
        {
            std::lock_guard<std::mutex> lock(mutexForFreeList_);
            if (freeList_ != nullptr)
            {
                Slot* temp = freeList_;
                freeList_ = freeList_->next;
                return temp;
            }
        }//离开作用域 → lock_guard析构 → 自动调用 mutexForFreeList_.unlock() 解锁
    }

    Slot* temp;
    {   
        std::lock_guard<std::mutex> lock(mutexForBlock_);
        if (curSlot_ >= lastSlot_)
        {
            // 当前内存块已无内存槽可用，开辟一块新的内存
            allocateNewBlock();
        }
    
        temp = curSlot_;
        // 这里不能直接 curSlot_ += SlotSize_ 因为curSlot_是Slot*类型，所以需要除以SlotSize_再加1
        curSlot_ += SlotSize_ / sizeof(Slot);
    }
    
    return temp; 
}

void MemoryPool::deallocate(void* ptr)
{
    if (ptr)
    {
        // 回收内存，将内存通过头插法插入到空闲链表中
        std::lock_guard<std::mutex> lock(mutexForFreeList_);
        reinterpret_cast<Slot*>(ptr)->next = freeList_;
        freeList_ = reinterpret_cast<Slot*>(ptr);
    }
}

void MemoryPool::allocateNewBlock()
{   
    //std::cout << "申请一块内存块，SlotSize: " << SlotSize_ << std::endl;
    // 头插法插入新的内存块
    void* newBlock = operator new(BlockSize_);
    reinterpret_cast<Slot*>(newBlock)->next = firstBlock_;
    firstBlock_ = reinterpret_cast<Slot*>(newBlock);

    char* body = reinterpret_cast<char*>(newBlock) + sizeof(Slot); // 精准定位新申请内存块中，跳过 “内存块管理头节点（Slot）” 后的实际可用内存起始位置; sizeof(Slot) = 8B
                                                                    //char*：“字节级内存地址”—— 编译器知道它的最小操作单位是 1 字节，因此支持「以字节为单位的指针加减」，能精准控制内存偏移；        
    size_t paddingSize = padPointer(body, SlotSize_); // 计算对齐需要填充内存的大小
    curSlot_ = reinterpret_cast<Slot*>(body + paddingSize);//内存对齐之后的未被使用的起始位置槽

    // 超过该标记位置，则说明该内存块已无内存槽可用，需向系统申请新的内存块
    // lastSlot_指向当前内存块中最后能存储槽的位置标志，等于或大于这个位置就代表当前内存块不足构成一个槽位以再用于存储对象了，所以要+1
    lastSlot_ = reinterpret_cast<Slot*>(reinterpret_cast<size_t>(newBlock) + BlockSize_ - SlotSize_ + 1);

    freeList_ = nullptr;
}

// 让指针对齐到槽大小的倍数位置
size_t MemoryPool::padPointer(char* p, size_t align)
{
    // align 是槽大小
    return (align - reinterpret_cast<size_t>(p)) % align;
}

void HashBucket::initMemoryPool()
{
    for (int i = 0; i < MEMORY_POOL_NUM; i++)
    {
        getMemoryPool(i).init((i + 1) * SLOT_BASE_SIZE); //SlotSize_最小为8,依次为上一个的二倍，参考STL Allocater
    }
}   

// 单例模式
MemoryPool& HashBucket::getMemoryPool(int index)
{
    static MemoryPool memoryPool[MEMORY_POOL_NUM]; //64个内存池，每一个Block就是一个内存池
    return memoryPool[index];
}

} // namespace memoryPool
