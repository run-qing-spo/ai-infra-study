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
    // std::vector<std::function<void>> 
    lru_mutex::lrucache_mutex<int, int> cache(4);
    for (std::size_t i = 0; i < pidNums*2; ++i) {
        std::mt19937 rng(i);
        int k = rng() % key_space;
        int v = k;
        int op = rng() % op_choices;
        pids.emplace_back([=, &cache]{
            for (int j = 0; j < 10000; ++j) {
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
    EXPECT_LE(vs.size(), 4u);
}