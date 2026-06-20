#pragma once
#include "lru_base.hpp"
#include <cstddef> // 这个头提供std::size_t
#include <mutex>
#include <vector>

namespace lru_mutex {
    template<typename K, typename V>
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

        private:
            mutable std::mutex mu;
            lru_base::lrucache_base<K, V> lru_base_;
    };
}