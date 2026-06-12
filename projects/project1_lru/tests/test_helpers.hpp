#pragma once
// Minimal test framework — macro names align with Google Test for easy migration.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <functional>

namespace test_detail {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

inline int& fail_count() {
    static int n = 0;
    return n;
}

inline int& current_fail_count() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

} // namespace test_detail

// --- Registration macro ---
#define TEST(name)                                                              \
    void test_##name();                                                         \
    static ::test_detail::Registrar reg_##name(#name, test_##name);             \
    void test_##name()

// --- Assertion macros ---
#define EXPECT_EQ(a, b)                                                        \
    do {                                                                        \
        if ((a) != (b)) {                                                       \
            std::printf("  FAIL %s:%d: EXPECT_EQ(%s, %s)\n",                    \
                        __FILE__, __LINE__, #a, #b);                            \
            ::test_detail::current_fail_count()++;                              \
        }                                                                       \
    } while (0)

#define EXPECT_NE(a, b)                                                        \
    do {                                                                        \
        if ((a) == (b)) {                                                       \
            std::printf("  FAIL %s:%d: EXPECT_NE(%s, %s)\n",                    \
                        __FILE__, __LINE__, #a, #b);                            \
            ::test_detail::current_fail_count()++;                              \
        }                                                                       \
    } while (0)

#define EXPECT_TRUE(cond)                                                      \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("  FAIL %s:%d: EXPECT_TRUE(%s)\n",                      \
                        __FILE__, __LINE__, #cond);                             \
            ::test_detail::current_fail_count()++;                              \
        }                                                                       \
    } while (0)

#define EXPECT_FALSE(cond)                                                     \
    do {                                                                        \
        if (cond) {                                                             \
            std::printf("  FAIL %s:%d: EXPECT_FALSE(%s)\n",                     \
                        __FILE__, __LINE__, #cond);                             \
            ::test_detail::current_fail_count()++;                              \
        }                                                                       \
    } while (0)

// ASSERT_* — fatal: return from current test on failure
#define ASSERT_EQ(a, b)                                                        \
    do {                                                                        \
        if ((a) != (b)) {                                                       \
            std::printf("  FATAL %s:%d: ASSERT_EQ(%s, %s)\n",                   \
                        __FILE__, __LINE__, #a, #b);                            \
            ::test_detail::current_fail_count()++;                              \
            return;                                                             \
        }                                                                       \
    } while (0)

#define ASSERT_TRUE(cond)                                                      \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("  FATAL %s:%d: ASSERT_TRUE(%s)\n",                     \
                        __FILE__, __LINE__, #cond);                             \
            ::test_detail::current_fail_count()++;                              \
            return;                                                             \
        }                                                                       \
    } while (0)

// --- Auto-running main ---
inline int run_all_tests() {
    int total = 0, passed = 0;
    for (auto& tc : ::test_detail::registry()) {
        ::test_detail::current_fail_count() = 0;
        std::printf("[ RUN  ] %s\n", tc.name);
        tc.fn();
        total++;
        if (::test_detail::current_fail_count() == 0) {
            std::printf("[ PASS ] %s\n", tc.name);
            passed++;
        } else {
            std::printf("[ FAIL ] %s (%d assertion(s) failed)\n",
                        tc.name, ::test_detail::current_fail_count());
            ::test_detail::fail_count()++;
        }
    }
    std::printf("\n%d/%d test(s) passed.\n", passed, total);
    return ::test_detail::fail_count() > 0 ? 1 : 0;
}

// main() lives in tests/test_main.cpp — `main` cannot be `inline` per the
// C++ standard, so putting it in a header risks a multiple-definition error
// the moment a test binary gains a second .cpp. Keeping it in a single TU
// (linked into every test binary) is the safe form.
