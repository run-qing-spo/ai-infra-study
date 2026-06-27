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
// 上一轮 (cache line 拆分):
//   head_ / tail_ 各自 alignas(kCacheLine), 让它们落在独立 cache line,
//   避免生产者写 head_ 和消费者写 tail_ 时反复抢同一条 cache line (false sharing).
//   Apple Silicon (arm64) L1 cache line 是 128 字节, x86 是 64, 这里取 128 兼容.
//   实测中等争用 (cap=64/1024) 加速 ~25%, cap=4 加速 ~15%.
//
// 上一轮 (cached head/tail):
//   · cached_tail_ 紧贴 head_, 同 cache line (生产者侧)
//   · cached_head_ 紧贴 tail_, 同 cache line (消费者侧)
//   Fast path: 用 cached_* 先判 "可能满 / 空", 命中时直接跳过跨核 atomic load.
//   只有缓存判 "满 / 空" 才回 slow path 刷新缓存并重判.
//   正确性: real_tail_ ≥ cached_tail_ (单调推进), 所以 next != cached_tail_
//          ⇒ next != real_tail_, "缓存判不满" 不会漏掉 "实际已满" 的情况.
//   实测中等 cap 再省 8-19%.
//
// 上一轮 (位运算 wrap):
//   要求 capacity 必须是 2 的幂. mask_ = cap_ - 1, 把 % cap_ 替换成 & mask_.
//   原因: cap_ 是运行时 size_t, 不是编译期常量, 现状 % 编译成真的 div 指令,
//        热路径上每次 push/pop 都执行一次 div, 是确定可消的开销.
//   代价: 容量参数受限 (必须 2 的幂). 构造时 assert 保底.
//   实测各 cap 再省 16-22%.
//
// 本轮 (move 语义) 已加:
//   · push 增加 T&& 右值重载, 与 const T& 共享同一份 push_impl (内部模板).
//     左值 → 拷贝赋值, 右值 → 移动赋值, 由 std::forward<U> 区分.
//   · pop 用 out = std::move(buf_[t]), 把槽位里的 T 资源转出来, 不再深拷贝.
//   动机: T 是大对象 (std::string / std::vector / unique_ptr) 时, 一次完整
//        push→pop 周期原本要走 "用户对象 → 槽位 → out" 两次深拷贝.
//        move 之后大型堆数据只搬指针, 不动堆内容, 大对象场景吞吐显著抬升.
//   兼容性: T 是平凡类型 (int 等) 时, move 自动退化为 copy, 无负面影响.
//          move-only 类型 (unique_ptr 等) 现在可以放进队列了.
//   moved-from 槽位: 处于 valid-but-unspecified 状态, 直到下一轮 push 把它覆盖.
//        要求 T 的 copy/move assignment 能从该状态接收新值 (标准库类型都满足).
//
// 学习笔记: 第一次单独加 cached (cache line 还没拆) 反而变慢 +30%~+72%, 因为
//   head_/tail_/cached_* 挤在同一 cache line, 写 cached_* 让对方 cache 失效,
//   没省下任何 cross-core miss 反而多了写动作. cache line 拆分是 cached 的地基.
//
// 所有计划内优化已收口. 后续若要继续, 可能方向 (按价值排, 不在本项目范围):
//   · 错误处理 API (assert → 异常 / Result), 让构造方知道容量参数非法
//   · pop 返回 std::optional<T> 而不是 out 参数 (代价: 多一次 move 构造)
//   · emplace 接口, 直接在槽位构造 T, 省掉 1 次 move assignment

#include <atomic>
#include <vector>
#include <cstddef>
#include <cassert>
#include <utility>  // std::forward, std::move

namespace spsc {

template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(size_t capacity)
        : buf_(capacity), cap_(capacity), mask_(capacity - 1) {
        // 要求 capacity 是 2 的幂 (>=2), 这是位运算 wrap (& mask_) 的前提.
        //   capacity & (capacity - 1) == 0  ⇔  capacity 是 2 的幂
        //   再 && >= 2: 容量 1 留一格当空隙, 实际能放 0 个, 没意义.
        // 现在用 assert 保底; 等讨论好错误处理策略 (异常 / Result API) 再换.
        assert(capacity >= 2 && (capacity & (capacity - 1)) == 0
               && "SpscQueue: capacity must be a power of two and >= 2");
    }

    // --- 拷贝禁掉 ---
    // 原子下标无法安全拷贝, 且 SPSC 语义本就不该被复制 (谁是 producer?)
    SpscQueue(const SpscQueue&)            = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // 生产者线程调用。塞不下返回 false, 不阻塞。
    // 两个明确重载, 用户无需懂 forwarding reference 就能用:
    //   push(const T&) : 左值, 拷贝进队列
    //   push(T&&)      : 右值, 把资源 move 进队列 (常见: push(std::move(big_obj)))
    // 共享逻辑放在 private push_impl 模板里, 见下方.
    bool push(const T& item) { return push_impl(item); }
    bool push(T&& item)      { return push_impl(std::move(item)); }

    // 消费者线程调用。空了返回 false, 不阻塞。
    bool pop(T& out) {
        // tail_ 只有消费者写, 自己读自己: relaxed
        const size_t t = tail_.load(std::memory_order_relaxed);

        // Fast path: 先用本地缓存判 "可能空", 命中时跳过跨核 acquire load head_.
        //   cached_head_ 和 tail_ 同 cache line (消费者侧), 读它是本地访问.
        if (t == cached_head_) {
            // Slow path: 缓存说空, 才去 atomic 看真实 head_, 顺便刷新缓存.
            // head_.load: acquire
            //   与生产者 push 末尾的 head_.store(release) 配对.
            //   看到 head_ 已推进 ⇒ 生产者对 buf_[t] 的写 happens-before
            //   下面我对它的读. 没这层保证就可能读到半写状态.
            cached_head_ = head_.load(std::memory_order_acquire);
            if (t == cached_head_) {
                return false; // 真空
            }
        }

        // 用 std::move 把槽位里的 T 资源转出来:
        //   T 是平凡类型 (int) → 退化为拷贝, 无副作用.
        //   T 是大对象 (string / vector) → 只搬指针, 不动堆内容.
        //   T 是 move-only (unique_ptr) → 槽位回 nullptr, 资源转交 out.
        // 之后 buf_[t] 进 moved-from 状态 (valid-but-unspecified),
        // 等下一轮该格被 push_impl 覆盖时, T 的赋值能从此状态接收新值.
        // 上面的 acquire 已经把 "读到生产者写的数据" 保住了.
        out = std::move(buf_[t]);

        // tail_.store: release
        //   通知生产者: buf_[t] 这一格我已读完.
        //   上面对 buf_[t] 的读不能被重排到这个 store 之后,
        //   生产者 tail_.load(acquire) 看到新值时, 就可以安全覆盖这一格.
        tail_.store((t + 1) & mask_, std::memory_order_release); // 同 push: & 替 %
        return true;
    }

    // 声明容量 (实际可放元素数为 cap_ - 1, 因为要留 1 格空隙)
    size_t capacity() const { return cap_; }

    // 近似元素数。跨线程读不精确 (调用过程中对方可能正在改),
    // 仅用于测试 / 调试 / benchmark 报数, 不要在业务逻辑里依赖它做判断。
    size_t size_approx() const {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_acquire);
        // cap_ 是 2 的幂: (h - t) & mask_ 在 size_t 下 wrap 的算术也对,
        // 等价于 (h + cap_ - t) % cap_, 但不需要做模运算.
        return (h - t) & mask_;
    }

private:
    // push 的实现, 模板让左值 / 右值共用一份代码:
    //   U 在调用点被推导:
    //     push(const T& item) → push_impl<const T&>(item)  → forward 出 const T& → 拷贝
    //     push(T&& item)      → push_impl<T>(std::move(item)) → forward 出 T&& → move
    //   std::forward<U> 保留左 / 右值类别, 不引入额外拷贝.
    template <typename U>
    bool push_impl(U&& item) {
        // head_ 只有生产者写, 自己读自己: relaxed (cache coherence 就够)
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t next = (h + 1) & mask_; // cap_ 是 2 的幂, 用位运算代替 % cap_

        // Fast path: 先用本地缓存判 "可能满", 命中时跳过跨核 acquire load tail_.
        //   cached_tail_ 和 head_ 同 cache line (生产者侧), 读它是本地访问.
        if (next == cached_tail_) {
            // Slow path: 缓存说满, 才去 atomic 看真实 tail_, 顺便刷新缓存.
            // tail_.load: acquire
            //   与消费者 pop 末尾的 tail_.store(release) 配对.
            //   看到 tail_ 已推进 ⇒ 消费者对 buf_[那一格] 的读 happens-before
            //   下面我对它的写. 否则就是同地址 "消费者读 vs 生产者写" 的 data race.
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (next == cached_tail_) {
                return false; // 真满 (留 1 格空隙, 见顶部不变量)
            }
        }

        // 普通赋值: 左值 → 拷贝, 右值 → move.
        // 下面的 release 会拦住, 这次写不会被推到 store 之后.
        buf_[h] = std::forward<U>(item);

        // head_.store: release
        //   发布数据. 上面对 buf_[h] 的写不能被重排到这个 store 之后.
        //   消费者用 head_.load(acquire) 看到 next 时, 与本 release 配对,
        //   就一定能看到 buf_[h] 的新内容.
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Cache line 大小:
    //   x86_64: 64
    //   Apple Silicon (arm64) / 部分 ARM 服务器: 128
    //   取 128 对两边都安全 —— 浪费一些 padding, 但不会让 false sharing 漏过。
    //   不直接用 std::hardware_destructive_interference_size, 因为 GCC/Clang
    //   在不同平台报的值不一致, 还会触发 -Winterference-size 警告。
    static constexpr size_t kCacheLine = 128;

    std::vector<T> buf_;
    size_t cap_;
    size_t mask_; // = cap_ - 1, 用于位运算 wrap; 构造后只读, 不参与 false sharing

    // 生产者侧 cache line (生产者读写, 消费者完全不碰):
    //   head_         : 真实下标, release 写让消费者看到
    //   cached_tail_  : 消费者 tail_ 的本地缓存, fast path 判满用 (普通 size_t)
    // 二者都只有生产者访问, 同一 cache line 不会 false sharing.
    // 关键: cached_tail_ 必须紧贴 head_, 否则它可能落进消费者 cache line ——
    //       那就是 "上次 cached 单独做" 失败的成因.
    alignas(kCacheLine) std::atomic<size_t> head_{0};
    size_t cached_tail_{0};

    // 消费者侧 cache line (消费者读写, 生产者完全不碰):
    //   tail_         : 真实下标, release 写让生产者看到
    //   cached_head_  : 生产者 head_ 的本地缓存, fast path 判空用
    alignas(kCacheLine) std::atomic<size_t> tail_{0};
    size_t cached_head_{0};
};

} // namespace spsc
