// test_spsc_concurrent.cpp — 多线程并发测试 (1 Producer + 1 Consumer)
//
// 为什么单独开一个文件:
//   单线程测试 (test_spsc_basic.cpp) 只覆盖 "算法逻辑" 层的 bug
//   (wrap / 留一格 / FIFO 顺序), 碰不到 spsc_queue.hpp 里那对 release-acquire
//   的内存可见性语义。要把那一层真的验证到, 只能开多线程, 而且要配合
//   ThreadSanitizer 一起跑:
//       make test-tsan
//   注意: 在 x86 这种强内存模型上, release/acquire 即使写成 relaxed,
//   普通跑也很可能 "巧合通过", 只有 TSan 才会把潜在 data race 揭出来。
//
// 设计思路:
//   - SPSC 语义只允许 1P1C, 没必要测多生产者 / 多消费者
//   - 生产者推 0, 1, 2, ..., N-1
//   - 消费者拉 N 个, 验证收到的序列严格等于 0, 1, ..., N-1
//     => 一次性覆盖 "不丢 / 不重 / 不乱序 / 不读到半写"
//   - 用 busy spin (而不是 sleep / yield) 反复撞两个原子, 给 TSan
//     最多的机会捕捉 race
//   - 死锁保底: Makefile 外层有 perl alarm, 真死锁会被 SIGALRM 强杀,
//     不会挂到 CI / watchdog

#include "spsc_queue.hpp"
#include <gtest/gtest.h>
#include <thread>

// 1. 主测试: 大数据量 + 中等容量, 验证 FIFO + 完整性。
//    任何重排 / 丢失 / 重复 / 读到半写, 第一处出错就 ASSERT 失败。
TEST(SpscQueueConcurrent, SingleProducerSingleConsumerOrder) {
    constexpr int N = 1'000'000;
    spsc::SpscQueue<int> q(1024);

    std::thread prod([&] {
        for (int i = 0; i < N; ++i) {
            while (!q.push(i)) {
                // 满了立刻重试 —— SPSC 典型用例就是低延迟管道, 不 yield
            }
        }
    });

    std::thread cons([&] {
        int v = -1;
        for (int i = 0; i < N; ++i) {
            while (!q.pop(v)) {
                // 空了立刻重试, 等生产者推进 head_
            }
            ASSERT_EQ(v, i) << "FIFO 破坏: 第 " << i << " 个应是 " << i
                            << ", 实际收到 " << v;
        }
    });

    prod.join();
    cons.join();

    EXPECT_EQ(q.size_approx(), 0u);
}

// 2. 极小容量 + 不算小的数据量: 让队列频繁打满 / 打空,
//    把 push 末尾的 release 和 pop 末尾的 release 反复触发,
//    生产者的 acquire(tail_) 和消费者的 acquire(head_) 也跟着反复触发。
//    cap=4 实际能放 3 个, N=100k 推完意味着两边互相等了非常多次。
TEST(SpscQueueConcurrent, TinyQueueBigStream) {
    constexpr int N = 100'000;
    spsc::SpscQueue<int> q(4);

    std::thread prod([&] {
        for (int i = 0; i < N; ++i) {
            while (!q.push(i)) {}
        }
    });

    std::thread cons([&] {
        int v = -1;
        for (int i = 0; i < N; ++i) {
            while (!q.pop(v)) {}
            ASSERT_EQ(v, i);
        }
    });

    prod.join();
    cons.join();
    EXPECT_EQ(q.size_approx(), 0u);
}
