// test_spsc_basic.cpp — 单线程功能测试 (骨架阶段)
//
// 现在 push/pop 都是占位 `return false;`, 这里挑了 3 条最基础的:
//   - ConstructAndCapacity        : 不依赖 push/pop, 现在就应该通过
//   - PopFromEmptyReturnsFalse    : pop 占位返回 false, 巧合也是对的, 通过
//   - PushThenPopReturnsSameValue : 依赖 push 真的能塞进去, 现在会失败
//
// 三个测试合起来证明: 测试管线已经打通 (gtest 链接 OK, 测试发现 OK,
// 失败时定位准确), 同时给了一个明确的"红"目标, 等实现完了它会自动变绿。

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
    // v 没被改写也算契约一部分, 不过现在 pop 根本没动 out, 这条不强求,
    // 等实现稳定后再加 EXPECT_EQ(v, -1)
}

TEST(SpscQueueBasic, PushThenPopReturnsSameValue) {
    spsc::SpscQueue<int> q(8);
    EXPECT_TRUE(q.push(42));   // 现在会失败: push 占位 return false
    int v = 0;
    EXPECT_TRUE(q.pop(v));
    EXPECT_EQ(v, 42);
}
