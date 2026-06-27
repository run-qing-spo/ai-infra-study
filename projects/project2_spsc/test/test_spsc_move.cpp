// test_spsc_move.cpp — push/pop 的 move 语义覆盖
//
// 现有 test_spsc_basic / test_spsc_concurrent 都用 int / int64_t,
// move 在 trivial 类型上退化为 copy, 看不出 move 路径有没有真的接通.
// 这个文件专门覆盖 move 相关的契约:
//
//   1. MoveOnlyTypeCompiles
//        unique_ptr<int> 没拷贝构造 / 赋值, 旧实现 (buf_[h] = item) 编译都过不去.
//        现在 push(T&&) + pop 用 std::move, 应当能编译通过且行为正确.
//
//   2. RvaluePushEmptiesSource
//        std::string s = "..."; q.push(std::move(s));
//        语义保证: 之后 s 应处于 moved-from 状态 (对 string 通常是 empty).
//        如果 push(T&&) 路径 "假动作" 走了拷贝, 这里就会失败.
//
//   3. LvaluePushPreservesSource
//        std::string s = "..."; q.push(s); // 左值
//        s 必须仍持有原内容. 是 const T& 重载不被 T&& 重载误捕的回归保险.
//
//   4. PopMovesOutOfSlot
//        push 进一个堆分配 (string), pop 出来; pop 后再 push 同一格,
//        新 push 不应当读到旧 string 的状态 —— buf_[h] = ... 必须能从
//        moved-from 状态接收新值. (标准库类型都满足这个契约, 这里是契约回归测试.)
//
// 这些测试都是单线程, 不需要 TSan. 并发场景下的 move 行为由
// test_spsc_concurrent 隐含覆盖 (那里跑 int64 不会出 move 问题, 但只要 push/pop
// 的 release/acquire 配对没变, 换 T 不影响并发正确性).

#include "spsc_queue.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <utility>

TEST(SpscQueueMove, MoveOnlyTypeCompiles) {
    spsc::SpscQueue<std::unique_ptr<int>> q(8);

    EXPECT_TRUE(q.push(std::make_unique<int>(42)));
    EXPECT_TRUE(q.push(std::make_unique<int>(99)));

    std::unique_ptr<int> out;
    EXPECT_TRUE(q.pop(out));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, 42);

    EXPECT_TRUE(q.pop(out));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, 99);

    EXPECT_FALSE(q.pop(out));
}

TEST(SpscQueueMove, RvaluePushEmptiesSource) {
    spsc::SpscQueue<std::string> q(8);

    // 用长 string 让小对象优化 (SSO) 不掩盖 move 痕迹:
    // 多数标准库实现里, 短 string 不分配堆, move 之后源 string 不一定为空.
    std::string s(64, 'x');
    const char* original_data = s.data(); // 记下堆指针

    EXPECT_TRUE(q.push(std::move(s)));

    // moved-from string 通常 (但标准只承诺 valid-but-unspecified) 为 empty.
    // 对 libstdc++ / libc++ 都是 empty, 这条断言在主流实现上稳定.
    EXPECT_TRUE(s.empty()) << "右值 push 后源 string 应被 move 走";

    std::string out;
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out.size(), 64u);
    EXPECT_EQ(out, std::string(64, 'x'));

    // 主流实现下: 资源指针应当一路 string s → buf_[h] → out 转交,
    // 中间不经历堆分配复制. 不强行 assert (允许实现细节差异), 仅参考.
    (void)original_data;
}

TEST(SpscQueueMove, LvaluePushPreservesSource) {
    spsc::SpscQueue<std::string> q(8);

    std::string s(64, 'y');
    EXPECT_TRUE(q.push(s)); // 左值, 走 const T& 重载, 必须拷贝

    EXPECT_EQ(s.size(), 64u) << "左值 push 不能掏空源 string";
    EXPECT_EQ(s, std::string(64, 'y'));

    std::string out;
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out, std::string(64, 'y'));
}

TEST(SpscQueueMove, PopMovesOutOfSlot) {
    // 容量 2 实际能放 1 个: 第 1 次 push 占用 buf_[0], pop 走后 buf_[0] 进
    // moved-from 状态; 第 2 次 push 必须能正确覆盖该状态. 跑多轮放大覆盖.
    spsc::SpscQueue<std::string> q(2);

    for (int round = 0; round < 50; ++round) {
        std::string s = "round-" + std::to_string(round) + std::string(50, '.');
        EXPECT_TRUE(q.push(std::move(s)));

        std::string out;
        EXPECT_TRUE(q.pop(out));
        EXPECT_EQ(out.substr(0, 6), "round-");
        EXPECT_EQ(out.size(), 6u + std::to_string(round).size() + 50u);
    }
}
