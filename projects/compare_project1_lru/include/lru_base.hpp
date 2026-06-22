#pragma once
// #include <cstddef>
// #include <cstdint>
// #include <cstdlib>
// #include <memory>
// #include <vector>
#include <unordered_map>
#include <type_traits>
#include <stdexcept>

namespace lru_base {

    template<typename K, typename V>
    class lrucache_base {
        static_assert(std::is_default_constructible_v<K>, "K must be default-constructible");
        public:
            explicit lrucache_base(int32_t x):_capacity(x), _head_free(0), used_head_sentinel_(x), used_tail_sentinel_(x+1){
                // 在这个函数体内部可以用_capacity了，列表初始化结束后才会执行函数体中的内容
                if (x <= 0) {
                    throw std::invalid_argument("capacity must > 0");
                }
                pool.reserve(static_cast<std::size_t>(x+2));
                for(int i = 0; i < x; ++i) {
                    pool.emplace_back(i+1, i-1);
                }
                pool[x-1].next = free_tail_sentinel_;
                pool.emplace_back(used_tail_sentinel_, used_head_sentinel_);
                pool.emplace_back(used_tail_sentinel_, used_head_sentinel_);
            }

            ~lrucache_base() {
                // 使用智能指针就不需要自己delete了
            }
            void push(const K& key, std::shared_ptr<V> value) {
                auto it = key2idx.find(key);
                if (it != key2idx.end()) {
                    pool[it->second].value = std::move(value);
                    moveToHead(it->second);
                    return;
                }
                if (_head_free == free_tail_sentinel_) {
                    auto tail = pool[used_tail_sentinel_].prev;
                    erase(pool[tail].key);
                }
                
                int32_t addPage = _head_free;
                pool[addPage].key = key;
                key2idx[key] = addPage;
                _head_free = pool[_head_free].next;
                pool[addPage].value = std::move(value);
                insertAtHead(addPage);
            }

            std::shared_ptr<V> get(const K& key) {
                auto it = key2idx.find(key);
                if (it != key2idx.end()) {
                    int32_t idx = it->second;
                    moveToHead(idx);
                    return pool[idx].value;
                }
                return nullptr;
            }

            void erase(const K& key) {
                auto it = key2idx.find(key);
                if (it != key2idx.end()) {
                    int32_t idx = it->second;
                    eraseByIdx(idx);
                }
            }

            std::vector<std::shared_ptr<V>> values() {
                std::vector<std::shared_ptr<V>> res;
                int32_t cur = pool[used_head_sentinel_].next;
                while(cur != used_tail_sentinel_) {
                    res.emplace_back(pool[cur].value);
                    cur = pool[cur].next;
                }
                return res;
            }

        private:
            int32_t _capacity;
            struct Node {
                K key;
                std::shared_ptr<V> value; // 存为指针，这样初始化方便填0
                int32_t next; // 存idx
                int32_t prev;
                Node(int32_t _nxt, int32_t _prv):value(nullptr), next(_nxt), prev(_prv){}
            };

            std::vector<Node> pool;
            std::unordered_map<K, int32_t> key2idx;

            int32_t _head_free;
            const int32_t used_head_sentinel_;
            const int32_t used_tail_sentinel_;
            static constexpr int32_t free_tail_sentinel_ = -1;

            void eraseByIdx(int32_t idx) {
                key2idx.erase(pool[idx].key);
                pool[pool[idx].next].prev = pool[idx].prev;
                pool[pool[idx].prev].next = pool[idx].next;
                pool[idx].value.reset();
                pool[idx].next = _head_free;
                _head_free = idx;
            }

            void insertAtHead(int32_t addPage) {
                // addPage变为新头
                pool[pool[used_head_sentinel_].next].prev = addPage; // 原来的头往前指向addPage
                pool[addPage].next = pool[used_head_sentinel_].next; // addPage往后指向原来头
                pool[used_head_sentinel_].next = addPage; // 哨兵头往后指向addpage
                pool[addPage].prev = used_head_sentinel_; // addPage往前指向哨兵头
            }
            void moveToHead(int32_t addPage) {
                // 断开addPage两端
                pool[pool[addPage].next].prev = pool[addPage].prev;
                pool[pool[addPage].prev].next = pool[addPage].next;
                // addPage变为新头
                insertAtHead(addPage);
            }
    };
}