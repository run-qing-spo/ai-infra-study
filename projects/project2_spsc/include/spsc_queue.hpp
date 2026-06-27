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
// 本轮范围 (骨架阶段): 已完成
// 当前轮: 实现 push/pop 的 memory_order 选择
//   核心两条 release-acquire 配对:
//     (a) 生产者写 buf_[h] → head_.store(release)
//         消费者 head_.load(acquire) → 读 buf_[t]
//         => 消费者看到新 head ⇒ 看到 buf_ 新数据
//     (b) 消费者读 buf_[t] → tail_.store(release)
//         生产者 tail_.load(acquire) → 写 buf_[h]
//         => 生产者看到新 tail ⇒ 那一格已被消费者读完, 可以安全覆盖
//   自己读自己写的那一侧 (push 读 head_, pop 读 tail_) 用 relaxed.
//
// 仍留待后续单独讨论, 不要现在塞进来:
//   · head_/tail_ 是否要 alignas(64) 拆 cache line (false sharing)
//   · wrap 用 % 还是位运算 (要求 cap 为 2 的幂)
//   · 是否需要在生产者本地缓存 tail_ / 消费者本地缓存 head_ (减少跨核 load)

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
    bool push(const T& item) {
        // head_ 只有生产者写, 自己读自己: relaxed (cache coherence 就够)
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t next = (h + 1) % cap_;

        // tail_.load: acquire
        //   与消费者 pop 末尾的 tail_.store(release) 配对.
        //   看到 tail_ 已推进 ⇒ 消费者对 buf_[那一格] 的读 happens-before
        //   下面我对它的写. 否则就是同地址 "消费者读 vs 生产者写" 的 data race.
        if (next == tail_.load(std::memory_order_acquire)) {
            return false; // 满 (留 1 格空隙, 见顶部不变量)
        }

        buf_[h] = item; // 普通写; 下面的 release 会拦住, 它不会被推到 store 之后

        // head_.store: release
        //   发布数据. 上面对 buf_[h] 的写不能被重排到这个 store 之后.
        //   消费者用 head_.load(acquire) 看到 next 时, 与本 release 配对,
        //   就一定能看到 buf_[h] 的新内容.
        head_.store(next, std::memory_order_release);
        return true;
    }

    // 消费者线程调用。空了返回 false, 不阻塞。
    bool pop(T& out) {
        // tail_ 只有消费者写, 自己读自己: relaxed
        const size_t t = tail_.load(std::memory_order_relaxed);

        // head_.load: acquire
        //   与生产者 push 末尾的 head_.store(release) 配对.
        //   看到 head_ 已推进 ⇒ 生产者对 buf_[t] 的写 happens-before
        //   下面我对它的读. 没这层保证就可能读到半写状态.
        if (t == head_.load(std::memory_order_acquire)) {
            return false; // 空
        }

        out = buf_[t]; // 普通读; 上面的 acquire 已经把"读到生产者写的数据"保住了

        // tail_.store: release
        //   通知生产者: buf_[t] 这一格我已读完.
        //   上面对 buf_[t] 的读不能被重排到这个 store 之后,
        //   生产者 tail_.load(acquire) 看到新值时, 就可以安全覆盖这一格.
        tail_.store((t + 1) % cap_, std::memory_order_release);
        return true;
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
