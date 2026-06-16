#include "lru_base.hpp"

#include <gtest/gtest.h>

#include <vector>

TEST(LruCacheBaseTest, PushAddsNodesAfterHead) {
    std::vector<int> values{1, 2, 4, 3};
    lru_base::lrucache_base<int, int> node(4);

    for (int value : values) {
        node.push(value, std::make_shared<int>(std::move(value))); 
    }

    std::vector<int> actual;
    auto shared_actual = node.show();
    for (const auto& v:shared_actual) {
        actual.emplace_back(*v);
    }

    const std::vector<int> expected{3, 4, 2, 1};
    EXPECT_EQ(expected, actual);
}
