#include <thread>
#include <atomic>
#include <vector>
#include <random>
#include <chrono>

TEST(LruMutexTest, ConcurrentMix) {
    constexpr int kThreads = 8;
    constexpr int kKeyRange = 200;
    constexpr int kCapacity = 64;

    lru_mutex::lrucache_mutex<int, int> cache(kCapacity);
    std::atomic<bool> start{false};   // ① barrier：让所有线程同时起跑
    std::atomic<bool> stop{false};    // ② 计时器到点统一收
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(t);       // ③ 每线程独立 RNG，避免共享 race
            while (!start.load(std::memory_order_acquire)) { /* spin */ }
            while (!stop.load(std::memory_order_relaxed)) {
                int op  = rng() % 3;
                int key = rng() % kKeyRange;
                if (op == 0) cache.push(key, std::make_shared<int>(key));
                else if (op == 1) cache.get(key);
                else cache.erase(key);
            }
        });
    }

    start.store(true, std::memory_order_release);                      // 起跑
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop.store(true, std::memory_order_relaxed);
    for (auto& th : threads) th.join();

    // 跑完之后检查不变量
    auto vs = cache.values();
    EXPECT_LE(vs.size(), static_cast<size_t>(kCapacity));
}