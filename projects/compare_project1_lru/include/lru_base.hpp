#pragma once
#include <cstdint>
#include <memory>
#include<optional>
#include<vector>
#include<utility>
#include <unordered_map>
#include <assert.h>

namespace lru_base {

    template<typename K, typename V>
    class lrucache_base {
        public:
            explicit lrucache_base(int32_t x):_capacity(x), _head_free(0), used_head_sentinel_(x), used_tail_sentinel_(x+1){
                // 在这个函数体内部可以用_capacity了，列表初始化结束后才会执行函数体中的内容
                pool.reserve(x+2);
                for(int i = 0; i < x; ++i) {
                    pool.emplace_back(i+1, i-1);
                }
                pool.emplace_back(used_tail_sentinel_, used_head_sentinel_);
                pool.emplace_back(used_tail_sentinel_, used_head_sentinel_);
            }

            ~lrucache_base() {
                // 使用智能指针就不需要自己delete了
            }
            void push(const K& key, std::shared_ptr<V> value) {
                if (key2idx.find(key) != key2idx.end()) {
                    return;
                }
                if (_head_free == _capacity) {
                    auto tail = pool[_capacity+1].prev;
                    erase(pool[tail].key);
                }
                int32_t addPage = _head_free;
                _head_free = pool[_head_free].next;
                pool[addPage].key = key;
                pool[addPage].value = value;
                key2idx[key] = addPage;
                moveToHead(addPage);
            }

            std::shared_ptr<V> get(const K& key) {
                if (key2idx.find(key) != key2idx.end()) {
                    int idx = key2idx[key];
                    moveToHead(idx);
                    return pool[idx].value;
                }
                return nullptr;
            }

            void erase(const K& key) {
                if (key2idx.find(key) != key2idx.end()) {
                    int idx = key2idx[key];
                    eraseByIdx(idx);
                }
            }

            std::vector<std::shared_ptr<V>> show() {
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
                Node(std::shared_ptr<V> val, int32_t _nxt, int32_t _prv):value(val), next(_nxt), prev(_prv){}
            };

            std::vector<Node> pool;
            std::unordered_map<K, int32_t> key2idx;

            int32_t _head_free;
            const int32_t used_head_sentinel_;
            const int32_t used_tail_sentinel_;

            void eraseByIdx(int32_t idx) {
                pool[pool[idx].next].prev = pool[idx].prev;
                pool[pool[idx].prev].next = pool[idx].next;
                pool[idx].value.reset();
                pool[idx].next = _head_free;
                _head_free = idx;
                key2idx.erase(pool[idx].key);
            }

            void moveToHead(int32_t addPage) {
                pool[addPage].next = pool[used_head_sentinel_].next; // 当前下一位
                pool[used_head_sentinel_].next = addPage; // 头的下一位
                pool[pool[addPage].next].prev = addPage; // 下一位的上一位
                pool[addPage].prev = used_head_sentinel_;
            }
    };
}