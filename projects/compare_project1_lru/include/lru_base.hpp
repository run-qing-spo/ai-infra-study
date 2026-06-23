#pragma once
// #include <cstddef>
// #include <cstdint>
// #include <cstdlib>
// #include <memory>
// #include <vector>
#include <unordered_map>
#include <type_traits>
#include <stdexcept>

#ifdef LRU_TEST_HOOKS
#include <string>
#include <vector>
#endif

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

#ifdef LRU_TEST_HOOKS
            // 返回空串表示所有不变量成立,否则返回首个被违反的不变量描述。
            std::string audit() const {
                const int max_steps = _capacity + 2;

                std::vector<int32_t> used_fwd;
                int32_t cur = pool[used_head_sentinel_].next;
                int steps = 0;
                while (cur != used_tail_sentinel_) {
                    if (++steps > max_steps) return "used list forward: cycle or overflow";
                    if (cur < 0 || cur >= _capacity) return "used list forward: idx out of range";
                    used_fwd.push_back(cur);
                    cur = pool[cur].next;
                }

                std::vector<int32_t> used_bwd;
                cur = pool[used_tail_sentinel_].prev;
                steps = 0;
                while (cur != used_head_sentinel_) {
                    if (++steps > max_steps) return "used list backward: cycle or overflow";
                    if (cur < 0 || cur >= _capacity) return "used list backward: idx out of range";
                    used_bwd.push_back(cur);
                    cur = pool[cur].prev;
                }

                if (used_fwd.size() != used_bwd.size()) {
                    return "used list: forward/backward length mismatch";
                }
                for (size_t i = 0; i < used_fwd.size(); ++i) {
                    if (used_fwd[i] != used_bwd[used_bwd.size() - 1 - i]) {
                        return "used list: forward/backward order mismatch";
                    }
                }

                std::vector<int32_t> free_idxs;
                cur = _head_free;
                steps = 0;
                while (cur != free_tail_sentinel_) {
                    if (++steps > max_steps) return "free list: cycle or overflow";
                    if (cur < 0 || cur >= _capacity) return "free list: idx out of range";
                    free_idxs.push_back(cur);
                    cur = pool[cur].next;
                }

                std::vector<bool> seen(static_cast<size_t>(_capacity), false);
                for (int32_t i : used_fwd) {
                    if (seen[i]) return "duplicate idx in used list";
                    seen[i] = true;
                }
                for (int32_t i : free_idxs) {
                    if (seen[i]) return "idx appears in both used and free";
                    seen[i] = true;
                }
                for (int32_t i = 0; i < _capacity; ++i) {
                    if (!seen[i]) return "idx missing from both lists";
                }

                if (key2idx.size() != used_fwd.size()) {
                    return "key2idx size != used list length";
                }
                for (int32_t i : used_fwd) {
                    auto it = key2idx.find(pool[i].key);
                    if (it == key2idx.end()) return "used node key missing from key2idx";
                    if (it->second != i) return "key2idx maps key to wrong idx";
                }
                return "";
            }
#endif

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