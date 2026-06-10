// test_lru_basic.cpp — V1 (LRUCache) single-thread functional tests.

#include "lru/lrucache.hpp"
#include "test_helpers.hpp"

TEST(put_and_get) {
    LRUCache<int, int> cache(3);
    cache.put(1, 100);
    cache.put(2, 200);
    cache.put(3, 300);

    EXPECT_EQ(cache.get(1).value_or(0), 100);
    EXPECT_EQ(cache.get(2).value_or(0), 200);
    EXPECT_EQ(cache.get(3).value_or(0), 300);
}

TEST(get_miss_returns_nullopt) {
    LRUCache<int, int> cache(3);
    EXPECT_TRUE(!cache.get(999).has_value());
}

TEST(eviction_order) {
    LRUCache<int, int> cache(3);
    cache.put(1, 100);
    cache.put(2, 200);
    cache.put(3, 300);

    // Access order: 1,2,3 → all recent. Access 1 to make it most recent.
    cache.get(1);
    // Now LRU order: 2,3,1. Inserting 4 should evict 2.
    cache.put(4, 400);

    EXPECT_TRUE(!cache.get(2).has_value());  // evicted
    EXPECT_EQ(cache.get(1).value_or(0), 100);
    EXPECT_EQ(cache.get(3).value_or(0), 300);
    EXPECT_EQ(cache.get(4).value_or(0), 400);
}

TEST(update_existing_key) {
    LRUCache<int, int> cache(3);
    cache.put(1, 100);
    cache.put(1, 999);  // update

    EXPECT_EQ(cache.get(1).value_or(0), 999);
    EXPECT_EQ(cache.size(), 1u);
}

TEST(update_makes_key_recent) {
    LRUCache<int, int> cache(2);
    cache.put(1, 100);
    cache.put(2, 200);
    // LRU order: 1,2. Update 1 to make it recent.
    cache.put(1, 111);
    // LRU order: 2,1. Insert 3 should evict 2.
    cache.put(3, 300);

    EXPECT_TRUE(!cache.get(2).has_value());  // evicted
    EXPECT_EQ(cache.get(1).value_or(0), 111);
    EXPECT_EQ(cache.get(3).value_or(0), 300);
}

TEST(capacity_one) {
    LRUCache<int, int> cache(1);
    cache.put(1, 100);
    EXPECT_EQ(cache.get(1).value_or(0), 100);

    cache.put(2, 200);
    EXPECT_TRUE(!cache.get(1).has_value());  // evicted
    EXPECT_EQ(cache.get(2).value_or(0), 200);
}

TEST(erase_existing) {
    LRUCache<int, int> cache(5);
    cache.put(1, 100);
    EXPECT_TRUE(cache.erase(1));
    EXPECT_TRUE(!cache.get(1).has_value());
    EXPECT_EQ(cache.size(), 0u);
}

TEST(erase_nonexistent) {
    LRUCache<int, int> cache(5);
    EXPECT_FALSE(cache.erase(999));
}

TEST(contains) {
    LRUCache<int, int> cache(5);
    cache.put(1, 100);
    EXPECT_TRUE(cache.contains(1));
    EXPECT_FALSE(cache.contains(2));
}

TEST(size_tracking) {
    LRUCache<int, int> cache(10);
    EXPECT_EQ(cache.size(), 0u);

    cache.put(1, 100);
    cache.put(2, 200);
    EXPECT_EQ(cache.size(), 2u);

    cache.erase(1);
    EXPECT_EQ(cache.size(), 1u);
}

TEST(string_key_value) {
    LRUCache<std::string, std::string> cache(2);
    cache.put("hello", "world");
    cache.put("foo", "bar");

    EXPECT_EQ(cache.get("hello").value_or(""), "world");
    cache.put("baz", "qux");  // evicts "foo"
    EXPECT_TRUE(!cache.get("foo").has_value());
    EXPECT_EQ(cache.get("baz").value_or(""), "qux");
}
