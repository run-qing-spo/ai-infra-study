// test_lru_concurrent.cpp — V1 + V2 concurrent tests.
// Also used for ThreadSanitizer validation (-fsanitize=thread).

#include "lru/lrucache.hpp"
#include "lru/sharded_lrucache.hpp"
#include "test_helpers.hpp"

#include <thread>
#include <vector>
#include <atomic>
#include <cstdio>

// ---------- V1 (LRUCache) concurrent tests ----------

TEST(v1_concurrent_put_get) {
    LRUCache<int, int> cache(1000);
    const int num_threads = 16;
    const int ops_per_thread = 5000;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&cache, i]() {
            for (int j = 0; j < ops_per_thread; j++) {
                int key = (i * ops_per_thread + j) % 2000;
                if (j % 2 == 0) {
                    cache.put(key, i * 1000 + j);
                } else {
                    cache.get(key);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_TRUE(cache.size() <= 1000u);
}

TEST(v1_concurrent_same_key) {
    // All threads hit the same key — stress test for mutex contention.
    LRUCache<int, int> cache(100);
    const int num_threads = 16;
    const int ops_per_thread = 5000;
    std::atomic<int> put_count{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&cache, &put_count, i]() {
            for (int j = 0; j < ops_per_thread; j++) {
                if (j % 3 == 0) {
                    cache.put(42, i * 1000 + j);
                    put_count++;
                } else {
                    cache.get(42);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // Key 42 must still exist and cache size must be bounded
    EXPECT_TRUE(cache.get(42).has_value());
    EXPECT_TRUE(cache.size() <= 100u);
}

TEST(v1_concurrent_insert_eviction) {
    // Threads insert unique keys until eviction happens repeatedly.
    LRUCache<int, int> cache(500);
    const int num_threads = 8;
    const int ops_per_thread = 2000;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&cache, i]() {
            for (int j = 0; j < ops_per_thread; j++) {
                // Each thread uses a unique key range to avoid overlap
                int key = i * 10000 + j;
                cache.put(key, j);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_TRUE(cache.size() <= 500u);
}

// ---------- V2 (ShardedLRUCache) concurrent tests ----------

TEST(v2_concurrent_put_get) {
    ShardedLRUCache<int, int> cache(1000);
    const int num_threads = 16;
    const int ops_per_thread = 5000;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&cache, i]() {
            for (int j = 0; j < ops_per_thread; j++) {
                int key = (i * ops_per_thread + j) % 2000;
                if (j % 2 == 0) {
                    cache.put(key, i * 1000 + j);
                } else {
                    cache.get(key);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_TRUE(cache.size() <= 1000u);
}

TEST(v2_concurrent_same_key) {
    ShardedLRUCache<int, int> cache(100);
    const int num_threads = 16;
    const int ops_per_thread = 5000;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&cache, i]() {
            for (int j = 0; j < ops_per_thread; j++) {
                if (j % 3 == 0) {
                    cache.put(42, i * 1000 + j);
                } else {
                    cache.get(42);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_TRUE(cache.get(42).has_value());
    EXPECT_TRUE(cache.size() <= 100u);
}

TEST(v2_concurrent_insert_eviction) {
    // Use 512 = 64 * 8 so total capacity is exactly divisible by num_shards.
    // (Each shard gets ceil(total/64); if total isn't a multiple of 64,
    //  the effective capacity is slightly larger than total_capacity.)
    ShardedLRUCache<int, int> cache(512);
    const int num_threads = 8;
    const int ops_per_thread = 2000;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&cache, i]() {
            for (int j = 0; j < ops_per_thread; j++) {
                int key = i * 10000 + j;
                cache.put(key, j);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_TRUE(cache.size() <= 512u);
}
