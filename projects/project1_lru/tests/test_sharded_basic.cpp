// test_sharded_basic.cpp — V2 (ShardedLRUCache) single-thread functional tests.

#include "lru/sharded_lrucache.hpp"
#include "test_helpers.hpp"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sys/wait.h>
#include <unistd.h>
#include <type_traits>

struct HashKey {
    int value;
};

inline bool operator==(const HashKey& lhs, const HashKey& rhs) {
    return lhs.value == rhs.value;
}

namespace std {
template <>
struct hash<HashKey> {
    size_t operator()(const HashKey& key) const noexcept {
        return static_cast<size_t>(key.value);
    }
};
} // namespace std

namespace {

template <typename Fn>
bool process_dies(Fn fn) {
    pid_t pid = fork();
    if (pid == 0) {
        if (std::freopen("/dev/null", "w", stderr) == nullptr) {
            std::_Exit(127);
        }
        fn();
        std::_Exit(0);
    }
    if (pid < 0) {
        return false;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return false;
    }
    return WIFSIGNALED(status) ||
           (WIFEXITED(status) && WEXITSTATUS(status) != 0);
}

} // namespace

static_assert(!std::is_move_constructible<ShardedLRUCache<int, int>>::value,
              "ShardedLRUCache should not be movable");

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
    ShardedLRUCache<int, int> cache(128);
    for (int i = 0; i < 300; i++) {
        cache.put(i, i * 10);
    }
    EXPECT_TRUE(cache.size() <= 128u);
}

TEST(sharded_strict_total_capacity) {
    ShardedLRUCache<HashKey, int> cache(100);
    for (int i = 0; i < 200; i++) {
        cache.put(HashKey{i}, i);
    }
    EXPECT_EQ(cache.size(), 100u);
}

TEST(sharded_small_capacity_eviction) {
    ShardedLRUCache<HashKey, int> cache(3, 2);
    EXPECT_EQ(cache.num_shards(), 2u);

    cache.put(HashKey{0}, 100);
    cache.put(HashKey{2}, 200);
    cache.get(HashKey{0});       // make key 0 recent within shard 0
    cache.put(HashKey{4}, 400);  // shard 0 capacity is 2, evicts key 2

    EXPECT_TRUE(!cache.get(HashKey{2}).has_value());
    EXPECT_EQ(cache.get(HashKey{0}).value_or(0), 100);
    EXPECT_EQ(cache.get(HashKey{4}).value_or(0), 400);

    cache.put(HashKey{1}, 10);
    cache.put(HashKey{3}, 30);   // shard 1 capacity is 1, evicts key 1

    EXPECT_TRUE(!cache.get(HashKey{1}).has_value());
    EXPECT_EQ(cache.get(HashKey{3}).value_or(0), 30);
    EXPECT_EQ(cache.size(), 3u);
}

TEST(sharded_rejects_invalid_parameters) {
    EXPECT_TRUE(process_dies([]() {
        ShardedLRUCache<int, int> cache(0, 1);
        (void)cache;
    }));
    EXPECT_TRUE(process_dies([]() {
        ShardedLRUCache<int, int> cache(100, 0);
        (void)cache;
    }));
    EXPECT_TRUE(process_dies([]() {
        ShardedLRUCache<int, int> cache(100, 65);
        (void)cache;
    }));
    EXPECT_TRUE(process_dies([]() {
        ShardedLRUCache<int, int> cache(3, 4);
        (void)cache;
    }));
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
