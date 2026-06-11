#pragma once
// ShardedLRUCache — V2: sharded LRU cache with per-shard mutex.
// 64 shards, each holding an independent LRUCacheBase + std::mutex.
// alignas(64) prevents false sharing between adjacent shards.

#include "lrucache_base.hpp"
#include <mutex>
#include <array>
#include <optional>
#include <utility>
#include <functional>
#include <cassert>
#include <exception>  // std::terminate

template <typename K, typename V>
class ShardedLRUCache {
public:
    explicit ShardedLRUCache(size_t total_capacity,
                             size_t num_shards = kDefaultShards)
        : num_shards_(checked_num_shards(total_capacity, num_shards)) {
        // Distribute exact total capacity; first shards receive the remainder.
        const size_t base_capacity = total_capacity / num_shards_;
        const size_t extra = total_capacity % num_shards_;
        for (size_t i = 0; i < num_shards_; i++) {
            const size_t shard_capacity = base_capacity + (i < extra ? 1 : 0);
            shards_[i].cache = LRUCacheBase<K, V>(shard_capacity);
        }
    }

    std::optional<V> get(const K& key) {
        auto& shard = shard_for(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        return shard.cache.get(key);
    }

    void put(const K& key, const V& value) {
        auto& shard = shard_for(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.cache.put(key, value);
    }

    bool erase(const K& key) {
        auto& shard = shard_for(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        return shard.cache.erase(key);
    }

    bool contains(const K& key) {
        auto& shard = shard_for(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        return shard.cache.contains(key);
    }

    // Note: O(num_shards) — must lock every shard to compute total size.
    size_t size() {
        size_t total = 0;
        for (size_t i = 0; i < num_shards_; i++) {
            std::lock_guard<std::mutex> lock(shards_[i].mutex);
            total += shards_[i].cache.size();
        }
        return total;
    }

    size_t num_shards() const noexcept {
        return num_shards_;
    }

    // --- Copy: deleted -------------------------------------------------------
    // Shard contains std::mutex (non-copyable) and LRUCacheBase (copy-deleted).
    ShardedLRUCache(const ShardedLRUCache&) = delete;
    ShardedLRUCache& operator=(const ShardedLRUCache&) = delete;

    // --- Move: deleted -------------------------------------------------------
    // Shard is alignas(64) and contains std::mutex, making it non-move-
    // assignable.  std::array<Shard, 64> is therefore non-movable.  If move
    // semantics are needed in the future, replace std::array with
    // std::unique_ptr<Shard[]> or std::vector<Shard>.
    ShardedLRUCache(ShardedLRUCache&&) = delete;
    ShardedLRUCache& operator=(ShardedLRUCache&&) = delete;

private:
    static constexpr size_t kDefaultShards = 64;

    struct alignas(64) Shard {
        LRUCacheBase<K, V> cache{1};  // placeholder; overwritten in constructor
        std::mutex mutex;
    };

    static size_t checked_num_shards(size_t total_capacity, size_t num_shards) {
#ifndef NDEBUG
        assert(total_capacity > 0 && "ShardedLRUCache total capacity must be > 0");
        assert(num_shards > 0 && "ShardedLRUCache shard count must be > 0");
        assert(num_shards <= kDefaultShards && "ShardedLRUCache shard count exceeds fixed shard storage");
        assert(num_shards <= total_capacity && "ShardedLRUCache shard count must not exceed total capacity");
#else
        if (total_capacity == 0 || num_shards == 0 ||
            num_shards > kDefaultShards || num_shards > total_capacity) {
            std::terminate();
        }
#endif
        return num_shards;
    }

    Shard& shard_for(const K& key) {
        size_t idx = std::hash<K>{}(key) % num_shards_;
        return shards_[idx];
    }

    const Shard& shard_for(const K& key) const {
        size_t idx = std::hash<K>{}(key) % num_shards_;
        return shards_[idx];
    }

    size_t num_shards_;
    std::array<Shard, kDefaultShards> shards_;
};
