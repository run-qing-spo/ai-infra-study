#pragma once
#include "lru_base.hpp"
#include <cstddef> // 这个头提供std::size_t
#include <functional>     // std::hash, std::equal_to
#include <mutex>
#include <vector>

#ifdef LRU_TEST_HOOKS
#include <string>
#endif

namespace lru_mutex {
    template<
        typename K,
        typename V,
        typename Hash = std::hash<K>,
        typename KeyEqual = std::equal_to<K>>
    class lrucache_mutex{
        public:
            explicit lrucache_mutex(std::size_t capacity_):lru_base_(capacity_){}
            void push(const K& key, std::shared_ptr<V> value) {
                std::lock_guard<std::mutex> g(mu);
                lru_base_.push(key, std::move(value));
            }

            std::shared_ptr<V> get(const K& key) {
                std::lock_guard<std::mutex> g(mu);
                return lru_base_.get(key);
            }

            void erase(const K& key) {
                std::lock_guard<std::mutex> g(mu);
                lru_base_.erase(key);
            }

            std::vector<std::shared_ptr<V>> values() {
                std::lock_guard<std::mutex> g(mu);
                return lru_base_.values();
            }

#ifdef LRU_TEST_HOOKS
            std::string audit() const {
                std::lock_guard<std::mutex> g(mu);
                return lru_base_.audit();
            }
#endif

        private:
            mutable std::mutex mu;
            lru_base::lrucache_base<K, V, Hash, KeyEqual> lru_base_;
    };
}