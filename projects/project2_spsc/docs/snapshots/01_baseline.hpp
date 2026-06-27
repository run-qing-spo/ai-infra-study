#pragma once
// ============================================================
// 历史快照 v01_baseline — memory_order 配对, 无任何优化
// ============================================================
// 这是 SpscQueue 完成 release-acquire 语义、所有正确性测试通过、
// 但尚未做任何性能优化时的代码状态. bench 路径就从这里出发.
//
// 用途: 阅读 / 对照. 不要在编译单元里和 include/spsc_queue.hpp 同时引用,
// 它们用了同一个 namespace + 类名, 会重复定义.
//
// 后续优化阶段见同目录其他文件 / README.md.
//
// 本轮范围: 已完成
//   - push/pop 的 memory_order 配对
//
// 核心两条 release-acquire 配对:
//   (a) 生产者写 buf_[h] → head_.store(release)
//       消费者 head_.load(acquire) → 读 buf_[t]
//       => 消费者看到新 head ⇒ 看到 buf_ 新数据
//   (b) 消费者读 buf_[t] → tail_.store(release)
//       生产者 tail_.load(acquire) → 写 buf_[h]
//       => 生产者看到新 tail ⇒ 那一格已被消费者读完, 可以安全覆盖
// 自己读自己写的那一侧 (push 读 head_, pop 读 tail_) 用 relaxed.
//
// 仍留待后续讨论:
//   - head_/tail_ 是否要 alignas 拆 cache line (false sharing)
//   - wrap 用 % 还是位运算 (要求 cap 为 2 的幂)
//   - 是否需要本地缓存对方下标 (减少跨核 load)

#include <atomic>
#include <vector>
#include <cstddef>

namespace spsc {

template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(size_t capacity)
        : buf_(capacity), cap_(capacity) {
        // TODO: 校验 capacity >= 2
    }

    SpscQueue(const SpscQueue&)            = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    bool push(const T& item) {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t next = (h + 1) % cap_;

        if (next == tail_.load(std::memory_order_acquire)) {
            return false; // 满
        }

        buf_[h] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const size_t t = tail_.load(std::memory_order_relaxed);

        if (t == head_.load(std::memory_order_acquire)) {
            return false; // 空
        }

        out = buf_[t];
        tail_.store((t + 1) % cap_, std::memory_order_release);
        return true;
    }

    size_t capacity() const { return cap_; }

    size_t size_approx() const {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_acquire);
        return (h + cap_ - t) % cap_;
    }

private:
    std::vector<T> buf_;
    size_t cap_;

    // head_ 和 tail_ 紧挨着, 大概率在同一条 cache line —— false sharing 源头
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

} // namespace spsc
