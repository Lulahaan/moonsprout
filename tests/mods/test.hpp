#pragma once
// p2mods_selftest (M5.2): a minimal test registry, no Aurora, no SDL, no disc and no Nintendo data.
// Each tests/mods/*.cpp file registers cases with MODS_TEST(name) { ... } and checks with
// MODS_CHECK(cond) / MODS_CHECK_EQ(a, b). tests/mods/main.cpp runs every case and prints
// "p2mods_selftest: <n> passed, <m> failed"; the exit code is 0 only when nothing failed.
// Committed in step 0 as tests/mods/test.hpp; only core edits it.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace mods_test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void fail(const char* file, int line, const std::string& what) {
    ++failures();
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what.c_str());
}

} // namespace mods_test

#define MODS_TEST_CAT2(a, b) a##b
#define MODS_TEST_CAT(a, b) MODS_TEST_CAT2(a, b)
#define MODS_TEST(name)                                                                                    \
    static void MODS_TEST_CAT(mods_test_fn_, name)();                                                      \
    static mods_test::Register MODS_TEST_CAT(mods_test_reg_, name)(#name, MODS_TEST_CAT(mods_test_fn_, name)); \
    static void MODS_TEST_CAT(mods_test_fn_, name)()
#define MODS_CHECK(cond)                                                                                   \
    do {                                                                                                   \
        if (!(cond)) mods_test::fail(__FILE__, __LINE__, #cond);                                           \
    } while (0)
#define MODS_CHECK_EQ(a, b)                                                                                \
    do {                                                                                                   \
        if (!((a) == (b))) mods_test::fail(__FILE__, __LINE__, #a " == " #b);                              \
    } while (0)
