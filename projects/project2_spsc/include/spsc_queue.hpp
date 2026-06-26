#pragma once
// SpscQueue — Bounded Single-Producer Single-Consumer lock-free ring buffer.
//
// 翻译:有界 / 单生产者单消费者 / 不加锁的先进先出队列
//   - Bounded   : 固定容量, 满了 push 返回 false, 不动态扩容
//   - Single Producer : 只允许一个线程调 push
//   - Single Consumer : 只允许一个线程调 pop
//   - Lock-free : 用两个原子下标代替 mutex
//
// 不变量 (invariants):
//   - head_ 只由生产者写, 消费者只读
//   - tail_ 只由消费者写, 生产者只读
//   - buf_[head_] 永远是 "下一个要塞的格子"
//   - buf_[tail_] 永远是 "下一个要取的格子"
//   - head_ == tail_              ⇒ 空
//   - (head_ + 1) % cap_ == tail_ ⇒ 满 (留 1 格区分空 / 满)
//
// 本轮范围 (骨架阶段):
//   只搭出类的形状, push/pop 函数体是占位的 return false。
//   下列实现细节都留到后续单独讨论, 不要现在塞进来:
//     · push/pop 里 std::atomic 的 memory_order 选什么
//     · head_/tail_ 是否要 alignas(64) 拆 cache line (false sharing)
//     · wrap 用 % 还是位运算 (要求 cap 为 2 的幂)
//     · 是否需要在生产者本地缓存 tail_ / 消费者本地缓存 head_ (减少跨核 load)

#include <atomic>
#include <vector>
#include <cstddef>

namespace spsc {

template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(size_t capacity)
        : buf_(capacity), cap_(capacity) {
        // TODO: 校验 capacity >= 2 (容量 1 等于无:留一格当空隙, 实际能放 0 个)
        //       现在不抛, 等讨论好错误处理策略再加
    }

    // --- 拷贝禁掉 ---
    // 原子下标无法安全拷贝, 且 SPSC 语义本就不该被复制 (谁是 producer?)
    SpscQueue(const SpscQueue&)            = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // 生产者线程调用。塞不下返回 false, 不阻塞。
    bool push(const T& /*item*/) {
        // TODO(下一轮): 实现见 .hpp 顶部注释的不变量
        return false;
    }

    // 消费者线程调用。空了返回 false, 不阻塞。
    bool pop(T& /*out*/) {
        // TODO(下一轮): 实现见 .hpp 顶部注释的不变量
        return false;
    }

    // 声明容量 (实际可放元素数为 cap_ - 1, 因为要留 1 格空隙)
    size_t capacity() const { return cap_; }

    // 近似元素数。跨线程读不精确 (调用过程中对方可能正在改),
    // 仅用于测试 / 调试 / benchmark 报数, 不要在业务逻辑里依赖它做判断。
    size_t size_approx() const {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_acquire);
        return (h + cap_ - t) % cap_;
    }

private:
    std::vector<T> buf_;
    size_t cap_;

    // head_: 下一个要塞的格子下标, 只有生产者写
    // tail_: 下一个要取的格子下标, 只有消费者写
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

} // namespace spsc
