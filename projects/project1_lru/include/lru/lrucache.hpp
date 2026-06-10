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

    // --- Copy: deleted -------------------------------------------------------
    // std::mutex is non-copyable, and copying would also copy the internal
    // LRUCacheBase whose iterators would dangle (see lrucache_base.hpp).
    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;

    // --- Move: defaulted -----------------------------------------------------
    // Both LRUCacheBase (node-transfer, iterators stay valid) and std::mutex
    // are move-constructible.  The moved-from cache is left in a valid-but-
    // empty state; its mutex is in an unspecified-but-valid state per the
    // standard, so it can be destructed safely.
    LRUCache(LRUCache&&) = default;
    LRUCache& operator=(LRUCache&&) = default;

private:
    LRUCacheBase<K, V> base_;
    mutable std::mutex mutex_;
};
