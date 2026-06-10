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

template <typename K, typename V>
class ShardedLRUCache {
public:
    explicit ShardedLRUCache(size_t total_capacity,
                             size_t num_shards = kDefaultShards)
        : num_shards_(num_shards) {
        // Distribute capacity evenly; each shard gets ceil(total / num_shards)
        size_t per_shard = (total_capacity + num_shards - 1) / num_shards;
        for (size_t i = 0; i < num_shards; i++) {
            shards_[i].cache = LRUCacheBase<K, V>(per_shard);
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

private:
    static constexpr size_t kDefaultShards = 64;

    struct alignas(64) Shard {
        LRUCacheBase<K, V> cache{1};  // placeholder; overwritten in constructor
        std::mutex mutex;
    };

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
