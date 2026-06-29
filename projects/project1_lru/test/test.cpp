#include "lru_base.hpp"

#include <cstddef>
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <vector>

TEST(LruCacheBaseTest, PushAddsNodesAfterHead) {
    std::vector<int> values{1, 2, 4, 3};
    lru_base::lrucache_base<int, int> node(4);

    for (int value : values) {
        node.push(value, std::make_shared<int>(std::move(value))); 
    }

    std::vector<int> actual;
    auto shared_actual = node.values();
    for (const auto& v:shared_actual) {
        actual.emplace_back(*v);
    }

    const std::vector<int> expected{3, 4, 2, 1};
    EXPECT_EQ(expected, actual);
}

TEST(LruCacheBaseTest, GetMoveToHead) {
    lru_base::lrucache_base<int, int> node(4);
    std::vector<int> a{4, 3, 2, 1};
    for (auto v:a) {
        node.push(v, std::make_shared<int>(v));
    }
    auto val2 = node.get(2);
    ASSERT_NE(val2, nullptr);
    EXPECT_EQ(*val2, 2);
    auto tmp = node.values();
    std::vector<int> s;
    for (const auto& v:tmp) {
        s.push_back(*v);
    }
    const std::vector<int> expected{2, 1, 3, 4};
    EXPECT_EQ(s, expected);
}

TEST(LruCacheBaseTest, GetNotExist) {
    lru_base::lrucache_base<int, int> node(1);
    auto a = node.get(1);
    EXPECT_EQ(a, nullptr);
}

TEST(LruCacheBaseTest, EvictFullPages) {
    lru_base::lrucache_base<int, int> node(1);
    node.push(1, std::make_shared<int>(1));
    node.push(2, std::make_shared<int>(2));
    auto a = node.values();
    std::vector<int> s(a.size());
    for (size_t i = 0; i < a.size(); ++i) {
        s[i] = *a[i];
    }
    EXPECT_EQ(s, std::vector<int>{2});
    auto x = node.get(1);
    EXPECT_EQ(x, nullptr);
}

TEST(LruCacheBaseTest, PushSameKey) {
    lru_base::lrucache_base<int, int> node(4);
    node.push(1, std::make_shared<int>(1));
    node.push(1, std::make_shared<int>(2));
    auto a = node.values();
    std::vector<int> s(a.size());
    for (size_t i = 0; i < a.size(); ++i) {
        s[i] = *a[i];
    }
    EXPECT_EQ(s, std::vector<int>{2});
}

TEST(LruCacheBaseTest, useAfterErase) {
    lru_base::lrucache_base<int, int> node(4); // 为什么不需要new？
    node.push(1, std::make_shared<int>(1)); // shared_ptr不能直接转换，只能用make_shared吗？
    node.erase(1);
    node.push(2, std::make_shared<int>(2));
    auto a = node.values();
    std::vector<int> s(a.size());
    for (size_t i = 0; i < s.size(); ++i) {
        s[i] = *a[i];
    }
    std::vector<int> expect{2};
    EXPECT_EQ(s, expect);
}

TEST(LruCacheBaseTest, useAfterEvict) {
    lru_base::lrucache_base<int, int> node(1);
    node.push(1, std::make_shared<int>(1));
    auto cur = node.get(1);
    node.push(2, std::make_shared<int>(2));
    ASSERT_NE(cur, nullptr);
    EXPECT_EQ(*cur, 1);
}

TEST(LruCacheBaseTest, ConstructorThrowsOnNonPositiveCapacity) {
    EXPECT_THROW((lru_base::lrucache_base<int, int>(0)), std::invalid_argument);
    EXPECT_THROW((lru_base::lrucache_base<int, int>(-1)), std::invalid_argument);
}

TEST(LruCacheBaseTest, EraseNonExistingIsNoop) {
    lru_base::lrucache_base<int, int> c(2);
    c.push(1, std::make_shared<int>(1));
    c.erase(99);
    auto v = c.values();
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(*v[0], 1);
    // free list 仍可用
    c.push(2, std::make_shared<int>(2));
    auto v2 = c.values();
    ASSERT_EQ(v2.size(), 2u);
    EXPECT_EQ(*v2[0], 2);
    EXPECT_EQ(*v2[1], 1);
}

TEST(LruCacheBaseTest, GetMissDoesNotChangeOrder) {
    lru_base::lrucache_base<int, int> c(3);
    c.push(1, std::make_shared<int>(1));
    c.push(2, std::make_shared<int>(2));
    c.push(3, std::make_shared<int>(3));
    auto before = c.values();
    EXPECT_EQ(c.get(99), nullptr);
    auto after = c.values();
    ASSERT_EQ(before.size(), after.size());
    for (size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(*before[i], *after[i]);
    }
}

TEST(LruCacheBaseTest, PushSameKeyAtCapacityUpdatesValueWithoutEviction) {
    lru_base::lrucache_base<int, int> c(3);
    c.push(1, std::make_shared<int>(10));
    c.push(2, std::make_shared<int>(20));
    c.push(3, std::make_shared<int>(30));
    c.push(2, std::make_shared<int>(222));  // 装满后覆盖 key=2
    auto v = c.values();
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(*v[0], 222);
    EXPECT_EQ(*v[1], 30);
    EXPECT_EQ(*v[2], 10);
    auto g1 = c.get(1); ASSERT_NE(g1, nullptr); EXPECT_EQ(*g1, 10);
    auto g3 = c.get(3); ASSERT_NE(g3, nullptr); EXPECT_EQ(*g3, 30);
}

TEST(LruCacheBaseTest, EraseHead) {
    lru_base::lrucache_base<int, int> c(3);
    c.push(1, std::make_shared<int>(1));
    c.push(2, std::make_shared<int>(2));
    c.push(3, std::make_shared<int>(3));
    c.erase(3);  // head
    auto v = c.values();
    ASSERT_EQ(v.size(), 2u);
    EXPECT_EQ(*v[0], 2);
    EXPECT_EQ(*v[1], 1);
    c.push(4, std::make_shared<int>(4));
    auto v2 = c.values();
    ASSERT_EQ(v2.size(), 3u);
    EXPECT_EQ(*v2[0], 4);
    EXPECT_EQ(*v2[1], 2);
    EXPECT_EQ(*v2[2], 1);
}

TEST(LruCacheBaseTest, EraseTail) {
    lru_base::lrucache_base<int, int> c(3);
    c.push(1, std::make_shared<int>(1));
    c.push(2, std::make_shared<int>(2));
    c.push(3, std::make_shared<int>(3));
    c.erase(1);  // tail
    auto v = c.values();
    ASSERT_EQ(v.size(), 2u);
    EXPECT_EQ(*v[0], 3);
    EXPECT_EQ(*v[1], 2);
    c.push(4, std::make_shared<int>(4));
    auto v2 = c.values();
    ASSERT_EQ(v2.size(), 3u);
    EXPECT_EQ(*v2[0], 4);
    EXPECT_EQ(*v2[1], 3);
    EXPECT_EQ(*v2[2], 2);
}

TEST(LruCacheBaseTest, EraseMiddle) {
    lru_base::lrucache_base<int, int> c(3);
    c.push(1, std::make_shared<int>(1));
    c.push(2, std::make_shared<int>(2));
    c.push(3, std::make_shared<int>(3));
    c.erase(2);  // middle
    auto v = c.values();
    ASSERT_EQ(v.size(), 2u);
    EXPECT_EQ(*v[0], 3);
    EXPECT_EQ(*v[1], 1);
    c.push(4, std::make_shared<int>(4));
    auto v2 = c.values();
    ASSERT_EQ(v2.size(), 3u);
    EXPECT_EQ(*v2[0], 4);
    EXPECT_EQ(*v2[1], 3);
    EXPECT_EQ(*v2[2], 1);
}

TEST(LruCacheBaseTest, FreeListIntegrityAfterManyCycles) {
    constexpr int cap = 4;
    constexpr int cycles = 1000;
    lru_base::lrucache_base<int, int> c(cap);
    for (int i = 0; i < cap; ++i) {
        c.push(i, std::make_shared<int>(i));
    }
    // 反复 erase + push,持续从 free list 取出又归还
    for (int i = 0; i < cycles; ++i) {
        int evict_key = i % cap;
        c.erase(evict_key);
        int new_key = cap + i;  // 永不与现存 key 撞
        c.push(new_key, std::make_shared<int>(new_key));
    }
    auto v = c.values();
    EXPECT_EQ(v.size(), static_cast<size_t>(cap));
    for (const auto& sp : v) {
        ASSERT_NE(sp, nullptr);
    }
}

TEST(LruCacheBaseTest, CapacityOneEraseThenPush) {
    lru_base::lrucache_base<int, int> c(1);
    c.push(1, std::make_shared<int>(1));
    c.erase(1);
    EXPECT_TRUE(c.values().empty());
    c.push(2, std::make_shared<int>(2));
    auto v = c.values();
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(*v[0], 2);
}

TEST(LruCacheBaseTest, AuditHoldsAcrossOperations) {
    lru_base::lrucache_base<int, int> c(3);
    EXPECT_EQ(c.audit(), "");  // 空缓存
    c.push(1, std::make_shared<int>(1));
    EXPECT_EQ(c.audit(), "");
    c.push(2, std::make_shared<int>(2));
    c.push(3, std::make_shared<int>(3));
    EXPECT_EQ(c.audit(), "");  // 满
    c.push(4, std::make_shared<int>(4));  // 触发 evict
    EXPECT_EQ(c.audit(), "");
    c.erase(3);                            // 中间 erase
    EXPECT_EQ(c.audit(), "");
    c.erase(99);                           // 不存在
    EXPECT_EQ(c.audit(), "");
    c.get(2);                              // 移动到头
    EXPECT_EQ(c.audit(), "");
    c.push(2, std::make_shared<int>(222)); // 覆盖同 key
    EXPECT_EQ(c.audit(), "");
}

TEST(LruCacheBaseTest, AuditHoldsThroughHeavyChurn) {
    constexpr int cap = 4;
    constexpr int cycles = 1000;
    lru_base::lrucache_base<int, int> c(cap);
    for (int i = 0; i < cap; ++i) {
        c.push(i, std::make_shared<int>(i));
    }
    for (int i = 0; i < cycles; ++i) {
        c.erase(i % cap);
        c.push(cap + i, std::make_shared<int>(cap + i));
        if (i % 100 == 0) {
            ASSERT_EQ(c.audit(), "") << "audit failed at cycle " << i;
        }
    }
    EXPECT_EQ(c.audit(), "");
}

TEST(LruCacheBaseTest, KeyValueConsistencyAfterMixedOps) {
    // 借助 v == k 不变量,审计 key2idx 是否指向正确节点
    constexpr int cap = 8;
    lru_base::lrucache_base<int, int> c(cap);
    std::vector<int> seq{1, 2, 3, 4, 5, 6, 7, 8, 3, 9, 10, 2, 11, 12, 5};
    for (int k : seq) {
        c.push(k, std::make_shared<int>(k));
    }
    c.erase(11);
    c.erase(2);
    int alive = 0;
    for (int k = 1; k <= 12; ++k) {
        auto sp = c.get(k);
        if (sp) {
            EXPECT_EQ(*sp, k) << "key " << k << " maps to wrong value";
            ++alive;
        }
    }
    EXPECT_EQ(c.get(11), nullptr);
    EXPECT_EQ(c.get(2), nullptr);
    EXPECT_EQ(static_cast<size_t>(alive), c.values().size());
    EXPECT_LE(c.values().size(), static_cast<size_t>(cap));
}

// === Layer 2: 鲁棒性 ===
//
// 当前实现观察到的异常安全保证 (用 audit() 验证不变量始终成立):
//   * 构造: capacity <= 0 抛 invalid_argument,所有已构造成员被正常析构。
//   * push 新 key + 容量未满: K 拷贝赋值若抛,旧条目原封不动,
//     容量未缩水 (strong-like)。
//   * push 新 key + 容量已满: K 拷贝赋值抛出前 evict 已发生,
//     LRU 项丢失但新项未进入 (basic exception safety, 非 strong)。
//   * erase / get: 仅触及 hash/equal,假设它们 noexcept。

namespace robust_test {
struct Throwy {
    int v;
    static inline thread_local int countdown = -1;  // -1 = 关闭注入
    Throwy() : v(0) {}
    explicit Throwy(int x) : v(x) {}
    Throwy(const Throwy&) = default;
    Throwy(Throwy&&) = default;
    Throwy& operator=(const Throwy& o) {
        tick();
        v = o.v;
        return *this;
    }
    Throwy& operator=(Throwy&&) = default;
    bool operator==(const Throwy& o) const noexcept { return v == o.v; }
private:
    static void tick() {
        if (countdown < 0) return;
        if (countdown == 0) {
            countdown = -1;
            throw std::runtime_error("Throwy injected");
        }
        --countdown;
    }
};

// Hasher 作为模板参数传给 lru_base,不需要进 std 名字空间
struct ThrowyHash {
    size_t operator()(const Throwy& t) const noexcept {
        return std::hash<int>{}(t.v);
    }
};
}  // namespace robust_test

TEST(LruBaseRobustness, PushThrowOnKeyAssignNotFullPreservesEverything) {
    using K = robust_test::Throwy;
    lru_base::lrucache_base<K, int, robust_test::ThrowyHash> c(3);
    c.push(K(1), std::make_shared<int>(1));
    ASSERT_EQ(c.audit(), "");

    K::countdown = 0;  // 让下一次 K 拷贝赋值抛出
    EXPECT_THROW(c.push(K(2), std::make_shared<int>(2)), std::runtime_error);
    K::countdown = -1;

    EXPECT_EQ(c.audit(), "");
    // 旧条目完整
    auto sp = c.get(K(1));
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(*sp, 1);
    // 新条目未进入
    EXPECT_EQ(c.get(K(2)), nullptr);
    // capacity 未缩:仍能再装两条
    c.push(K(2), std::make_shared<int>(2));
    c.push(K(3), std::make_shared<int>(3));
    EXPECT_EQ(c.values().size(), 3u);
    EXPECT_EQ(c.audit(), "");
}

TEST(LruBaseRobustness, PushThrowOnKeyAssignAtCapacityEvictsButDoesNotInsert) {
    // 暴露并钉住当前语义:满容量下 push 抛出时,LRU 项已被驱逐,
    // 即不满足 strong exception safety;但 audit 表明不变量仍然成立。
    using K = robust_test::Throwy;
    lru_base::lrucache_base<K, int, robust_test::ThrowyHash> c(2);
    c.push(K(1), std::make_shared<int>(1));  // LRU
    c.push(K(2), std::make_shared<int>(2));  // MRU
    ASSERT_EQ(c.audit(), "");

    K::countdown = 0;
    EXPECT_THROW(c.push(K(3), std::make_shared<int>(3)), std::runtime_error);
    K::countdown = -1;

    EXPECT_EQ(c.audit(), "");
    EXPECT_EQ(c.get(K(1)), nullptr);  // 已被驱逐
    auto sp = c.get(K(2));
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(*sp, 2);
    EXPECT_EQ(c.get(K(3)), nullptr);  // 未插入
}

TEST(LruBaseRobustness, ErasedSharedPtrStaysAlive) {
    lru_base::lrucache_base<int, int> c(2);
    c.push(1, std::make_shared<int>(42));
    auto sp = c.get(1);
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp.use_count(), 2);  // cache + 外部
    c.erase(1);
    EXPECT_EQ(c.get(1), nullptr);
    // erase 不应影响外部持有的 shared_ptr
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(*sp, 42);
    EXPECT_EQ(sp.use_count(), 1);  // cache 内部副本已释放
    EXPECT_EQ(c.audit(), "");
}