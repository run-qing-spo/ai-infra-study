#include "lru_base.hpp"

#include <gtest/gtest.h>

#include <vector>

TEST(LruCacheBaseTest, PushAddsNodesAfterHead) {
    std::vector<int> values{1, 2, 4, 3};
    lru_base::lrucache_base node(1);

    for (int value : values) {
        node.push(value);
    }

    std::vector<int> actual;
    for (lru_base::lrucache_base* cur = &node; cur != nullptr; cur = cur->next) {
        actual.emplace_back(cur->val);
    }

    const std::vector<int> expected{1, 3, 4, 2, 1};
    EXPECT_EQ(expected, actual);
}
