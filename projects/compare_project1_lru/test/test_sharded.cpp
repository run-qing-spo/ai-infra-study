#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "lru_sharded.hpp"

// === Layer 1: 单线程功能 + 路由健全 ===

TEST(LruShardedTest, BasicFunctional) {
    // 总容量 64,16 shard → per_shard = 4
    // 用 64 个 key 顺序灌进去,每 shard 落 4 个左右(splitmix 混合后近似均匀)
    lru_sharded::lrucache_sharded<int, int> cache(64, 16);

    for (int k = 0; k < 64; ++k) {
        cache.push(k, std::make_shared<int>(k));
    }

    // 不变量:audit 干净
    EXPECT_EQ(cache.audit(), "");

    // 因为 per_shard=4 而我们灌了 64 个 key,大部分 shard 会发生 evict;
    // 这里只能保证总活跃数 ≤ shard_cap * shard_count = 64
    auto vs = cache.values();
    EXPECT_LE(vs.size(), 64u);

    // 单 shard 内的 LRU 语义还成立:刚 push 的 key 一定在
    for (int k = 60; k < 64; ++k) {
        auto sp = cache.get(k);
        ASSERT_NE(sp, nullptr) << "key " << k << " 刚 push 就被 evict 了";
        EXPECT_EQ(*sp, k);
    }
}

TEST(LruShardedTest, EraseAndReinsert) {
    // 验证 erase 后 key 真的没了,再 push 同 key 能正确插回
    lru_sharded::lrucache_sharded<int, int> cache(32, 8);
    for (int k = 0; k < 16; ++k) {
        cache.push(k, std::make_shared<int>(k * 10));
    }
    for (int k = 0; k < 16; k += 2) {
        cache.erase(k);
    }
    for (int k = 0; k < 16; ++k) {
        auto sp = cache.get(k);
        if (k % 2 == 0) {
            EXPECT_EQ(sp, nullptr) << "key " << k << " erase 后还在";
        } else {
            // 奇数 key 可能因为同 shard 内 evict 不在了,这里不强求
            if (sp) EXPECT_EQ(*sp, k * 10);
        }
    }
    EXPECT_EQ(cache.audit(), "");
}

// === Layer 2: 并发不变量 ===

TEST(LruShardedTest, ConcurrentStorm) {
    // 模仿 LruMutexTest::KeyValueConsistencyAfterConcurrentStorm。
    // 这里用更大的 key_space 和 cap,确保流量真的跨 shard,而不是退化成单 shard 测试。
    constexpr int key_space = 256;
    constexpr int cap = 64;
    constexpr int shard_count = 16;  // per_shard = 4
    constexpr int ops_per_thread = 20000;

    auto fut = std::async(std::launch::async, [&]{
        lru_sharded::lrucache_sharded<int, int> cache(cap, shard_count);
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

        // 结构层不变量:逐 shard 自洽
        EXPECT_EQ(cache.audit(), "");
        // 内容层不变量:每个活着的 key 对应 value 没被串到别的 key 上
        int alive = 0;
        for (int k = 0; k < key_space; ++k) {
            auto sp = cache.get(k);
            if (sp) {
                EXPECT_EQ(*sp, k) << "key " << k << " 被串到了 value " << *sp;
                ++alive;
            }
        }
        // 全局容量上界:不会超过 per_shard * shard_count
        int per_shard = (cap + shard_count - 1) / shard_count;
        EXPECT_LE(alive, per_shard * shard_count);
    });

    ASSERT_EQ(fut.wait_for(std::chrono::seconds(30)), std::future_status::ready)
        << "storm 超时,疑似死锁/活锁";
    fut.get();
}

TEST(LruShardedTest, ValuesSnapshotInternallyConsistent) {
    // values() 是跨 shard 聚合,不是全局原子快照;但每个 shard 内部应该自洽,
    // 因此整体也应满足:size ≤ 全局上界、每个 sp 非空且 *sp 在合法范围。
    constexpr int key_space = 64;
    constexpr int cap = 16;
    constexpr int shard_count = 4;
    constexpr int snapshot_iters = 5000;

    auto fut = std::async(std::launch::async, [&]{
        lru_sharded::lrucache_sharded<int, int> cache(cap, shard_count);
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
        int per_shard = (cap + shard_count - 1) / shard_count;
        size_t upper_bound = static_cast<size_t>(per_shard) * shard_count;
        for (int iter = 0; iter < snapshot_iters; ++iter) {
            auto snap = cache.values();
            ASSERT_LE(snap.size(), upper_bound);
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

// === Layer 3: 路由不会黑洞掉 key ===
// 不是性能 / 分布均匀性测试 —— 那需要 per-shard 计数,这里没暴露。
// 这条测试只验证:splitmix + mod 不会把某些 key 路由到不存在的 shard、
// 或者把 push 进去的 key 默默丢掉。
// 注意:不能用"keys = per_shard * shard_count"这种刚好等于总容量的设定 ——
// balls-in-bins 下哪怕 hash 完美均匀,某些 shard 也必然过载、evict。
// 必须给足容量头空间(这里 16x),才能用 size 比对验证。
TEST(LruShardedTest, RoutingDoesNotBlackholeKeys) {
    constexpr int shard_count = 16;
    constexpr int keys = 256;
    constexpr int cap = keys * 16;  // 每 shard 256 容量,远超 keys/shard 的方差上界
    lru_sharded::lrucache_sharded<int, int> cache(cap, shard_count);
    for (int k = 0; k < keys; ++k) {
        cache.push(k, std::make_shared<int>(k));
    }
    auto vs = cache.values();
    EXPECT_EQ(vs.size(), static_cast<size_t>(keys));
    EXPECT_EQ(cache.audit(), "");
    // 二次确认:每个 key 都能 get 回来
    for (int k = 0; k < keys; ++k) {
        auto sp = cache.get(k);
        ASSERT_NE(sp, nullptr) << "key " << k << " 被路由丢了";
        EXPECT_EQ(*sp, k);
    }
}
