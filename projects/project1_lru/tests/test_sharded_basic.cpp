// test_sharded_basic.cpp — V2 (ShardedLRUCache) single-thread functional tests.

#include "lru/sharded_lrucache.hpp"
#include "test_helpers.hpp"

TEST(sharded_put_and_get) {
    ShardedLRUCache<int, int> cache(100);
    cache.put(1, 100);
    cache.put(2, 200);
    cache.put(3, 300);

    EXPECT_EQ(cache.get(1).value_or(0), 100);
    EXPECT_EQ(cache.get(2).value_or(0), 200);
    EXPECT_EQ(cache.get(3).value_or(0), 300);
}

TEST(sharded_get_miss) {
    ShardedLRUCache<int, int> cache(100);
    EXPECT_TRUE(!cache.get(999).has_value());
}

TEST(sharded_eviction) {
    // Total capacity 6 → each of 64 shards gets ceil(6/64)=1.
    // But we need multiple keys in the same shard for eviction.
    // Use a small total capacity and rely on hash collisions or
    // just verify that the total size stays bounded.
    ShardedLRUCache<int, int> cache(128);
    for (int i = 0; i < 300; i++) {
        cache.put(i, i * 10);
    }
    // Size should be bounded by total capacity
    EXPECT_TRUE(cache.size() <= 128u);
}

TEST(sharded_update_existing) {
    ShardedLRUCache<int, int> cache(100);
    cache.put(1, 100);
    cache.put(1, 999);
    EXPECT_EQ(cache.get(1).value_or(0), 999);
}

TEST(sharded_erase) {
    ShardedLRUCache<int, int> cache(100);
    cache.put(1, 100);
    EXPECT_TRUE(cache.erase(1));
    EXPECT_TRUE(!cache.get(1).has_value());
}

TEST(sharded_contains) {
    ShardedLRUCache<int, int> cache(100);
    cache.put(42, 420);
    EXPECT_TRUE(cache.contains(42));
    EXPECT_FALSE(cache.contains(99));
}

TEST(sharded_size) {
    ShardedLRUCache<int, int> cache(200);
    EXPECT_EQ(cache.size(), 0u);
    cache.put(1, 10);
    cache.put(2, 20);
    EXPECT_EQ(cache.size(), 2u);
}

TEST(sharded_string_key) {
    ShardedLRUCache<std::string, int> cache(100);
    cache.put("alpha", 1);
    cache.put("beta", 2);
    EXPECT_EQ(cache.get("alpha").value_or(0), 1);
    EXPECT_EQ(cache.get("beta").value_or(0), 2);
}
