#pragma once
// bench_helpers.hpp — lightweight benchmark utilities.
// ScopedTimer, LatencyCollector, and markdown table printer.

#include <chrono>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <numeric>
#include <cmath>

// ---- Timer ----

class ScopedTimer {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    ScopedTimer() : start_(Clock::now()) {}

    void reset() { start_ = Clock::now(); }

    // Returns elapsed nanoseconds since start/reset.
    double elapsed_ns() const {
        auto end = Clock::now();
        return std::chrono::duration<double, std::nano>(end - start_).count();
    }

    double elapsed_us() const { return elapsed_ns() / 1000.0; }
    double elapsed_ms() const { return elapsed_ns() / 1e6; }
    double elapsed_s()  const { return elapsed_ns() / 1e9; }

private:
    TimePoint start_;
};

// ---- Latency statistics ----

struct LatencyStats {
    double p50_ns;
    double p90_ns;
    double p99_ns;
    double max_ns;
    double min_ns;
    double avg_ns;
};

class LatencyCollector {
public:
    void reserve(size_t n) { samples_.reserve(n); }

    void add(uint64_t ns) { samples_.push_back(ns); }

    // const-lvalue overload: copies samples, leaves collector intact.
    LatencyStats compute() const & {
        std::vector<uint64_t> sorted = samples_;
        return compute_from_sorted(std::move(sorted));
    }

    // rvalue overload: sorts in place, avoiding the temporary copy.
    LatencyStats compute() && {
        return compute_from_sorted(std::move(samples_));
    }

    void merge(const LatencyCollector& other) {
        samples_.insert(samples_.end(), other.samples_.begin(), other.samples_.end());
    }

    size_t count() const { return samples_.size(); }

private:
    static LatencyStats compute_from_sorted(std::vector<uint64_t>&& xs) {
        if (xs.empty()) return {0, 0, 0, 0, 0, 0};
        std::sort(xs.begin(), xs.end());

        // p in [0,100] → idx in [0, xs.size()-1], no clamp needed.
        auto percentile = [&](double p) -> double {
            size_t idx = static_cast<size_t>(p / 100.0 * (xs.size() - 1));
            return static_cast<double>(xs[idx]);
        };

        double sum = std::accumulate(xs.begin(), xs.end(), 0.0);
        return LatencyStats{
            percentile(50),
            percentile(90),
            percentile(99),
            static_cast<double>(xs.back()),
            static_cast<double>(xs.front()),
            sum / xs.size()
        };
    }

    std::vector<uint64_t> samples_;
};

// ---- Result row ----

struct BenchResult {
    const char* label;     // e.g. "V1" or "V2"
    int threads;
    double throughput;      // ops/sec
    LatencyStats latency;
};

// ---- Markdown table printer ----

inline void print_results_table(const std::vector<BenchResult>& results) {
    std::printf("\n| Version | Threads | Throughput (Mops/s) | p50 (ns) | p99 (ns) | Max (ns) |\n");
    std::printf("|---------|---------|--------------------|----------|----------|----------|\n");
    for (const auto& r : results) {
        std::printf("| %-7s | %7d | %18.2f | %8.0f | %8.0f | %8.0f |\n",
                    r.label,
                    r.threads,
                    r.throughput / 1e6,
                    r.latency.p50_ns,
                    r.latency.p99_ns,
                    r.latency.max_ns);
    }
    std::printf("\n");
}

// ---- Per-thread latency sampler ----
// Records individual operation latencies with minimal overhead.
// alignas(64): adjacent samplers in std::vector<OpSampler> must not share a
// cache line; otherwise per-op push_back from different threads false-shares
// the vector header bytes and inflates measured tail latencies — defeating
// the whole point of comparing V1's global mutex against V2's sharded mutex.

class alignas(64) OpSampler {
public:
    void reserve(size_t n) { samples_.reserve(n); }

    void begin() { start_ = ScopedTimer::Clock::now(); }

    void end() {
        auto finish = ScopedTimer::Clock::now();
        uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start_).count());
        samples_.push_back(ns);
    }

    std::vector<uint64_t>& samples() { return samples_; }
    const std::vector<uint64_t>& samples() const { return samples_; }

private:
    ScopedTimer::TimePoint start_;
    std::vector<uint64_t> samples_;
};
