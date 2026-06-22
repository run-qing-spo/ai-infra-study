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