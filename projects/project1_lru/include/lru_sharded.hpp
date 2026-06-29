#pragma once
#include "lru_mutex.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

#ifdef LRU_TEST_HOOKS
#include <string>
#endif

namespace lru_sharded {

    // splitmix64 finalizer(Stafford 变种)。
    // 作用:把 std::hash<K> 的输出做雪崩混合,让低位带上高位的熵。
    // 为什么需要:std::hash<int> 在 libstdc++/libc++ 里是恒等函数,直接 % N
    //   只看低 log2(N) 位;遇到块对齐 id / 指针地址这类低位有结构的键时,
    //   所有"分片"都会塌到少数几个 shard 上,分片化解锁竞争的努力归零。
    // 代价:几纳秒 / 调用,对 P1 学习目标无影响。
    inline std::size_t splitmix_finalize(std::size_t h) {
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return h;
    }

    template<
        typename K,
        typename V,
        typename Hash = std::hash<K>,
        typename KeyEqual = std::equal_to<K>>
    class lrucache_sharded {
        public:
            // total_capacity:逻辑总容量。
            // shard_count:分片数;越大锁竞争越低,但每 shard 容量越小、热点 key 簇聚
            //   时局部 evict 越频繁,全局命中率退化越明显(这是分片副作用,不是 bug)。
            // 实际总容量 = ceil(total_capacity / shard_count) * shard_count,
            //   可能略大于请求值 —— 分片缓存通用的"全局容量近似"语义。
            explicit lrucache_sharded(std::size_t total_capacity, std::size_t shard_count = 64) {
                if (shard_count == 0) {
                    throw std::invalid_argument("shard_count must > 0");
                }
                if (total_capacity == 0) {
                    throw std::invalid_argument("total_capacity must > 0");
                }
                std::size_t per_shard = (total_capacity + shard_count - 1) / shard_count;

                // 每个 shard 堆上单独分配,vector 持有 unique_ptr。
                // 为什么不直接 vector<lrucache_mutex>:lrucache_mutex 内含 std::mutex,
                //   既不可拷贝也不可移动。libc++ 的 vector::reserve 在实例化时
                //   会静态断言 MoveInsertible(即使实际不会触发 move 也要求类型满足),
                //   导致编译失败。unique_ptr 满足 MoveInsertible,问题消失。
                // 代价:每次调用多一层指针解引用,纳秒级,不影响 P1 结论。
                shards_.reserve(shard_count);
                for (std::size_t i = 0; i < shard_count; ++i) {
                    shards_.emplace_back(
                        std::make_unique<ShardT>(per_shard));
                }
            }

            void push(const K& key, std::shared_ptr<V> value) {
                shard_for(key).push(key, std::move(value));
            }

            std::shared_ptr<V> get(const K& key) {
                return shard_for(key).get(key);
            }

            void erase(const K& key) {
                shard_for(key).erase(key);
            }

            // 跨 shard 聚合,逐 shard 取锁释放。结果是"逐 shard 一致、整体最终一致"的快照,
            // 不是全局原子快照 —— 持有所有 shard 锁代价过大,且违背分片的初衷。
            std::vector<std::shared_ptr<V>> values() {
                std::vector<std::shared_ptr<V>> res;
                for (auto& s : shards_) {
                    auto v = s->values();
                    res.insert(res.end(), v.begin(), v.end());
                }
                return res;
            }

#ifdef LRU_TEST_HOOKS
            // 任一 shard 报错就返回带 shard 编号的错误串;全部 OK 返回空串。
            std::string audit() const {
                for (std::size_t i = 0; i < shards_.size(); ++i) {
                    auto r = shards_[i]->audit();
                    if (!r.empty()) {
                        return "shard " + std::to_string(i) + ": " + r;
                    }
                }
                return "";
            }
#endif

        private:
            using ShardT = lru_mutex::lrucache_mutex<K, V, Hash, KeyEqual>;
            std::vector<std::unique_ptr<ShardT>> shards_;
            Hash hasher_;

            ShardT& shard_for(const K& key) {
                std::size_t h = splitmix_finalize(hasher_(key));
                return *shards_[h % shards_.size()];
            }
    };
}
