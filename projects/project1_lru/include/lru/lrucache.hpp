#pragma once
// LRUCache — V1: thread-safe LRU cache with a global mutex.
// Wraps LRUCacheBase with std::mutex for correctness baseline.

#include "lrucache_base.hpp"
#include <mutex>
#include <optional>
#include <utility>

template <typename K, typename V>
class LRUCache {
public:
    explicit LRUCache(size_t capacity) : base_(capacity) {}

    std::optional<V> get(const K& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        return base_.get(key);
    }

    void put(const K& key, const V& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        base_.put(key, value);
    }

    bool erase(const K& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        return base_.erase(key);
    }

    bool contains(const K& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        return base_.contains(key);
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return base_.size();
    }

    size_t capacity() const noexcept {
        return base_.capacity();
    }

private:
    LRUCacheBase<K, V> base_;
    mutable std::mutex mutex_;
};
