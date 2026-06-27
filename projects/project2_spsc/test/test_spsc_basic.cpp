// test_spsc_basic.cpp — 单线程功能测试 (骨架阶段)
//
// 现在 push/pop 都是占位 `return false;`, 这里挑了 3 条最基础的:
//   - ConstructAndCapacity        : 不依赖 push/pop, 现在就应该通过
//   - PopFromEmptyReturnsFalse    : pop 占位返回 false, 巧合也是对的, 通过
//   - PushThenPopReturnsSameValue : 依赖 push 真的能塞进去, 现在会失败
//
// 三个测试合起来证明: 测试管线已经打通 (gtest 链接 OK, 测试发现 OK,
// 失败时定位准确), 同时给了一个明确的"红"目标, 等实现完了它会自动变绿。
//
// 本轮 (push/pop 实现完之后) 追加的单线程边界用例:
//   - WrapAroundIndex         : 跨 cap_-1 → 0 那一圈, 验证 % cap_ 没写偏
//   - FillToCapacityMinusOne  : "留一格" 满判定, 声明 N 实际能放 N-1
//   - FifoOrderPreserved      : 多元素严格按入队顺序出
//   - CapacityTwoMinimum      : cap=2 退化场景, 每次 push 立刻满, 最容易暴露算错
//   - ManyWrapsStress         : 反复 wrap 多圈, 当 wrap 被改坏的回归网
//
// 注意: 这些都是 "算法逻辑" 层面的 bug, 错了不依赖并发就能复现; 内存可见性
// (release-acquire 配对) 不在这层测, 那是后续 TSan + 多线程测试的活。

#include "spsc_queue.hpp"
#include <gtest/gtest.h>

TEST(SpscQueueBasic, ConstructAndCapacity) {
    spsc::SpscQueue<int> q(8);
    EXPECT_EQ(q.capacity(), 8u);
    EXPECT_EQ(q.size_approx(), 0u);
}

TEST(SpscQueueBasic, PopFromEmptyReturnsFalse) {
    spsc::SpscQueue<int> q(8);
    int v = -1;
    EXPECT_FALSE(q.pop(v));
    EXPECT_EQ(v, -1);
}

TEST(SpscQueueBasic, PushThenPopReturnsSameValue) {
    spsc::SpscQueue<int> q(8);
    EXPECT_TRUE(q.push(42));
    int v = 0;
    EXPECT_TRUE(q.pop(v));
    EXPECT_EQ(v, 42);
}

// 1. wrap 一圈: 容量 4 实际能放 3 个。
//    填 3 → 取 2 → 再填 2 (此时 head 从 3 绕回 0) → 取完剩下 3 个。
//    重点验证下标绕回去之后值仍按入队顺序出来, 没串位也没丢。
TEST(SpscQueueBasic, WrapAroundIndex) {
    spsc::SpscQueue<int> q(4); // 实际能放 3
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_TRUE(q.push(3));

    int v = 0;
    EXPECT_TRUE(q.pop(v)); EXPECT_EQ(v, 1);
    EXPECT_TRUE(q.pop(v)); EXPECT_EQ(v, 2);

    // 此时 head_=3, tail_=2; 下一次 push, head_ 从 3 wrap 回 0
    EXPECT_TRUE(q.push(4));
    EXPECT_TRUE(q.push(5)); // 又满了 (head_=1, tail_=2)

    EXPECT_TRUE(q.pop(v)); EXPECT_EQ(v, 3);
    EXPECT_TRUE(q.pop(v)); EXPECT_EQ(v, 4);
    EXPECT_TRUE(q.pop(v)); EXPECT_EQ(v, 5);
    EXPECT_FALSE(q.pop(v)); // 又空
}

// 2. "留一格" 满判定: 声明容量 N, 实际只能装 N-1 个。
//    push N-1 次都应成功, 第 N 次必须失败;
//    pop 一次释放一格之后再 push 又能成功, 紧接着又满。
//    专门盯 (h+1)%cap_ == tail_ 这条公式有没有偏一个。
TEST(SpscQueueBasic, FillToCapacityMinusOne) {
    constexpr size_t N = 4;
    spsc::SpscQueue<int> q(N);
    for (size_t i = 0; i + 1 < N; ++i) {
        EXPECT_TRUE(q.push(static_cast<int>(i)))
            << "应能放下第 " << i << " 个";
    }
    EXPECT_FALSE(q.push(999)) << "第 N 次必须满: 留一格区分空/满";

    int v = 0;
    EXPECT_TRUE(q.pop(v));      // 释放一格
    EXPECT_TRUE(q.push(999));   // 又能塞了
    EXPECT_FALSE(q.push(1000)); // 立刻又满
}

// 3. FIFO 顺序: 队列的语义契约。单线程能测就别留到并发场景碰运气。
TEST(SpscQueueBasic, FifoOrderPreserved) {
    spsc::SpscQueue<int> q(16); // 实际能放 15
    constexpr int K = 10;
    for (int i = 0; i < K; ++i) {
        ASSERT_TRUE(q.push(i));
    }
    for (int i = 0; i < K; ++i) {
        int v = -1;
        ASSERT_TRUE(q.pop(v));
        EXPECT_EQ(v, i) << "FIFO: 第 " << i << " 个出来的应是 " << i;
    }
}

// 4. 极小容量 cap=2: 实际能放 1 个, 是 "留一格" 退化到极限。
//    每次 push 之后立刻满, pop 之后立刻空。
//    顺便回答顶部 spsc_queue.hpp TODO 里 "capacity >= 2" 的下限到底成不成立。
TEST(SpscQueueBasic, CapacityTwoMinimum) {
    spsc::SpscQueue<int> q(2);
    EXPECT_EQ(q.capacity(), 2u);

    for (int round = 0; round < 5; ++round) {
        EXPECT_TRUE(q.push(round));
        EXPECT_FALSE(q.push(round + 100)) << "cap=2 实际只能放 1 个";

        int v = -1;
        EXPECT_TRUE(q.pop(v));
        EXPECT_EQ(v, round);
        EXPECT_FALSE(q.pop(v)) << "拿完了应空";
    }
}

// 5. 反复 wrap 多圈: 在小容量上跑几千轮 push→pop, 让下标自然 wrap 很多次。
//    如果 1 (WrapAroundIndex) 通过, 这条大概率也通过, 但加上很便宜,
//    将来 wrap 实现被改 (比如改成位运算) 时, 是张及时报警的回归网。
TEST(SpscQueueBasic, ManyWrapsStress) {
    spsc::SpscQueue<int> q(4);
    constexpr int kRounds = 5000;
    for (int i = 0; i < kRounds; ++i) {
        ASSERT_TRUE(q.push(i));
        int v = -1;
        ASSERT_TRUE(q.pop(v));
        ASSERT_EQ(v, i);
    }
    EXPECT_EQ(q.size_approx(), 0u);
}
