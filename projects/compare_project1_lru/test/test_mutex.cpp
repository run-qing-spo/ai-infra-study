#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>
#include <thread>
#include "lru_mutex.hpp"
#include "gtest/gtest.h"

TEST(LruMutexTest, ConcurrentMix) {
    std::vector<std::thread> pids;
    constexpr int key_space = 20;
    constexpr int op_choices = 10;
    unsigned pidNums = std::thread::hardware_concurrency();
    lru_mutex::lrucache_mutex<int, int> cache(4);
    for (std::size_t i = 0; i < pidNums*2; ++i) {
        pids.emplace_back([i, &cache]{
            std::mt19937 rng(i);
            for (int j = 0; j < 10000; ++j) {
                int k = rng() % key_space;
                int v = k;
                int op = rng() % op_choices;
                switch(op) {
                    case 0: case 1: case 2: case 3: case 4:
                        cache.get(k); break;
                    case 5: case 6: case 7:
                        cache.push(k, std::make_shared<int>(v)); break;
                    case 8: case 9:
                        cache.erase(k); break;
                }
            }
            
        });
    }
    for (std::size_t i = 0; i < pidNums*2; ++i) {
        pids[i].join();
    }

    // 开始检查
    auto vs = cache.values();

    // 1. 容量不变量
    EXPECT_LE(vs.size(), 4u);

    // 2. key/value 合法：因为 v = k 且 k ∈ [0, key_space)，所有 value 应该落在该范围
    //    若读到范围外的值，说明数据被竞争撕裂
    for (const auto& sp : vs) {
        ASSERT_NE(sp, nullptr);
        EXPECT_GE(*sp, 0);
        EXPECT_LT(*sp, key_space);
    }

    // 3. join 后 cache 仍可用：用一个 stress 期间不会出现的 key 验证
    constexpr int sentinel_key = key_space + 100;
    cache.push(sentinel_key, std::make_shared<int>(sentinel_key));
    auto sp = cache.get(sentinel_key);
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(*sp, sentinel_key);
}

// === Layer 3: 并发不变量 ===

TEST(LruMutexTest, KeyValueConsistencyAfterConcurrentStorm) {
    // 在 v == k 的约定下,storm 结束后,所有活着的 key 对应的 *sp 必须等于 key 本身。
    // 这条断言抓的是「value 被串到别的 key 上」这种竞争撕裂,
    // 比 ConcurrentMix 里那条「v 在 [0, key_space)」要严得多。
    // 用 std::async + wait_for 兜底,防止死锁/活锁让测试无限挂死。
    constexpr int key_space = 32;
    constexpr int cap = 8;
    constexpr int ops_per_thread = 20000;

    auto fut = std::async(std::launch::async, [&]{
        lru_mutex::lrucache_mutex<int, int> cache(cap);
        unsigned thread_count = std::max(2u, std::thread::hardware_concurrency() * 2);
        std::vector<std::thread> ts;
        for (unsigned i = 0; i < thread_count; ++i) {
            ts.emplace_back([i, &cache]{
                std::mt19937 rng(i);
                for (int j = 0; j < ops_per_thread; ++j) {
                    int k = static_cast<int>(rng() % key_space);
                    int op = static_cast<int>(rng() % 10);
                    switch (op) {
                        case 0: case 1: case 2: case 3: case 4:
                            cache.get(k); break;
                        case 5: case 6: case 7:
                            cache.push(k, std::make_shared<int>(k)); break;
                        case 8: case 9:
                            cache.erase(k); break;
                    }
                }
            });
        }
        for (auto& t : ts) t.join();

        // 结构层不变量:unordered_map / free list / used list 互相对得上
        EXPECT_EQ(cache.audit(), "");
        // 内容层不变量:每个活着的 key 对应的 value 没被串
        int alive = 0;
        for (int k = 0; k < key_space; ++k) {
            auto sp = cache.get(k);
            if (sp) {
                EXPECT_EQ(*sp, k) << "key " << k << " 被串到了 value " << *sp;
                ++alive;
            }
        }
        EXPECT_LE(alive, cap);
    });

    ASSERT_EQ(fut.wait_for(std::chrono::seconds(30)), std::future_status::ready)
        << "storm 超时,疑似死锁/活锁";
    fut.get();  // 把线程内未捕获的异常重抛出来
}

TEST(LruMutexTest, ValuesSnapshotInternallyConsistent) {
    // 并发 push/erase 进行时,values() 拿到的快照必须自洽:
    //   size <= capacity 且每个 sp 非空且 *sp 在合法范围。
    // values() 当前是个临界区,这条测试为「未来谁拆锁优化」做回归保护。
    constexpr int key_space = 16;
    constexpr int cap = 4;
    constexpr int snapshot_iters = 5000;

    auto fut = std::async(std::launch::async, [&]{
        lru_mutex::lrucache_mutex<int, int> cache(cap);
        std::atomic<bool> stop{false};
        unsigned writer_count = std::max(2u, std::thread::hardware_concurrency());
        std::vector<std::thread> ts;
        for (unsigned i = 0; i < writer_count; ++i) {
            ts.emplace_back([i, &cache, &stop]{
                std::mt19937 rng(i);
                while (!stop.load(std::memory_order_relaxed)) {
                    int k = static_cast<int>(rng() % key_space);
                    if (rng() % 2) {
                        cache.push(k, std::make_shared<int>(k));
                    } else {
                        cache.erase(k);
                    }
                }
            });
        }
        for (int iter = 0; iter < snapshot_iters; ++iter) {
            auto snap = cache.values();
            ASSERT_LE(snap.size(), static_cast<size_t>(cap));
            for (const auto& sp : snap) {
                ASSERT_NE(sp, nullptr);
                EXPECT_GE(*sp, 0);
                EXPECT_LT(*sp, key_space);
            }
        }
        stop.store(true);
        for (auto& t : ts) t.join();
        EXPECT_EQ(cache.audit(), "");
    });

    ASSERT_EQ(fut.wait_for(std::chrono::seconds(30)), std::future_status::ready)
        << "快照测试超时";
    fut.get();
}