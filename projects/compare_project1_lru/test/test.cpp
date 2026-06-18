#include "lru_base.hpp"

#include <cstddef>
#include <gtest/gtest.h>

#include <memory>
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