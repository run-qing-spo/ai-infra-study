// bench_throughput.cpp — throughput + latency benchmark for V1 and V2 LRU caches.
// Measures ops/sec and p50/p99 latency at thread counts [1, 2, 4, 8, 16].

#include "lru/lrucache.hpp"
#include "lru/sharded_lrucache.hpp"
#include "bench_helpers.hpp"

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <atomic>
#include <functional>

// ---- Configuration ----

static constexpr int kCacheCapacity  = 10000;
static constexpr int kKeySpace       = 20000;   // keys in [0, kKeySpace)
static constexpr int kTotalOps       = 1000000;  // total operations per benchmark run
static constexpr int kWarmupOps      = 1000;
static constexpr int kThreadCounts[] = {1, 2, 4, 8, 16};

// ---- Generic benchmark runner ----

template <typename Cache>
BenchResult run_benchmark(const char* label, int num_threads, Cache& cache) {
    const int ops_per_thread = kTotalOps / num_threads;

    // Warmup
    for (int i = 0; i < kWarmupOps; i++) {
        cache.put(i % kKeySpace, i);
        cache.get(i % kKeySpace);
    }

    // Per-thread latency collectors
    std::vector<OpSampler> samplers(num_threads);
    for (auto& s : samplers) s.reserve(ops_per_thread);

    std::atomic<int> ready{0};
    std::atomic<int> go{0};

    auto worker = [&](int tid) {
        // Barrier: all threads start together
        ready.fetch_add(1);
        while (go.load() == 0) { /* spin */ }

        OpSampler& sampler = samplers[tid];
        for (int j = 0; j < ops_per_thread; j++) {
            int key = (tid * ops_per_thread + j) % kKeySpace;
            if (j % 2 == 0) {
                sampler.begin();
                cache.put(key, tid * 1000 + j);
                sampler.end();
            } else {
                sampler.begin();
                cache.get(key);
                sampler.end();
            }
        }
    };

    // Launch threads
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(worker, i);
    }

    // Wait for all threads to be ready, then start
    while (ready.load() < num_threads) { /* spin */ }
    ScopedTimer wall_timer;
    go.store(1);

    for (auto& t : threads) {
        t.join();
    }
    double wall_s = wall_timer.elapsed_s();

    // Merge latency samples
    LatencyCollector collector;
    for (auto& s : samplers) {
        for (auto ns : s.samples()) {
            collector.add(ns);
        }
    }

    double throughput = kTotalOps / wall_s;
    LatencyStats latency = collector.compute();

    return BenchResult{label, num_threads, throughput, latency};
}

// ---- Main ----

int main() {
    std::vector<BenchResult> results;

    for (int t : kThreadCounts) {
        // V1: global mutex
        {
            LRUCache<int, int> cache(kCacheCapacity);
            results.push_back(run_benchmark("V1", t, cache));
        }

        // V2: sharded
        {
            ShardedLRUCache<int, int> cache(kCacheCapacity);
            results.push_back(run_benchmark("V2", t, cache));
        }
    }

    std::printf("# Thread-safe LRU Cache Benchmark\n");
    std::printf("# Cache capacity: %d, Key space: %d, Ops/run: %d\n",
                kCacheCapacity, kKeySpace, kTotalOps);
    std::printf("# Workload: 50%% get, 50%% put\n\n");

    print_results_table(results);

    return 0;
}
