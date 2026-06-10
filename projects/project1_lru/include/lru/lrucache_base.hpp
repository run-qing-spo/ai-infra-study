#pragma once
// LRUCacheBase — non-thread-safe LRU cache core.
// Pure data structure: hash map + doubly linked list, O(1) get/put.
// Thread-safe wrappers (LRUCache, ShardedLRUCache) layer locking on top.

#include <list>
#include <unordered_map>
#include <optional>
#include <utility>
#include <cassert>
#include <exception>  // std::terminate

template <typename K, typename V>
class LRUCacheBase {
public:
    explicit LRUCacheBase(size_t capacity) : capacity_(capacity) {
        assert(capacity > 0 && "LRU capacity must be > 0");
        if (capacity == 0) {
            std::terminate();  // Safety net in release builds (NDEBUG)
        }
    }

    // --- Copy: deleted -------------------------------------------------------
    // map_ stores iterators into list_; copying list_ creates new nodes at
    // new addresses, but the copied map_ iterators would still point into the
    // *original* list — instant dangling iterators on any mutation.
    LRUCacheBase(const LRUCacheBase&) = delete;
    LRUCacheBase& operator=(const LRUCacheBase&) = delete;

    // --- Move: defaulted -----------------------------------------------------
    // std::list move transfers nodes in-place (splice); iterators remain valid
    // and now refer to the moved-to list.  The moved-from object is left in a
    // valid-but-empty state.
    LRUCacheBase(LRUCacheBase&&) = default;
    LRUCacheBase& operator=(LRUCacheBase&&) = default;

    // Returns the value if key exists, moves key to most-recent position.
    // Returns std::nullopt on miss.
    std::optional<V> get(const K& key) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return std::nullopt;
        }
        // Move accessed node to front (most recently used)
        list_.splice(list_.begin(), list_, it->second);
        return it->second->second;
    }

    // Inserts or updates a key-value pair. Moves key to most-recent position.
    // Evicts the least-recently-used entry if capacity is exceeded.
    void put(const K& key, const V& value) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            // Key exists: update value and move to front
            it->second->second = value;
            list_.splice(list_.begin(), list_, it->second);
            return;
        }

        // Evict LRU if at capacity
        if (list_.size() >= capacity_) {
            evict_lru();
        }

        // Insert new entry at front
        list_.emplace_front(key, value);
        map_[key] = list_.begin();
    }

    // Returns true and removes the key if it exists.
    bool erase(const K& key) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return false;
        }
        list_.erase(it->second);
        map_.erase(it);
        return true;
    }

    // Returns true if key exists (does NOT update recency).
    bool contains(const K& key) const {
        return map_.find(key) != map_.end();
    }

    size_t size() const noexcept {
        return list_.size();
    }

    size_t capacity() const noexcept {
        return capacity_;
    }

    bool empty() const noexcept {
        return list_.empty();
    }

private:
    void evict_lru() {
        if (list_.empty()) return;
        map_.erase(list_.back().first);
        list_.pop_back();
    }

    size_t capacity_;
    std::list<std::pair<K, V>> list_;                                    // front = MRU, back = LRU
    std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator> map_;
};
