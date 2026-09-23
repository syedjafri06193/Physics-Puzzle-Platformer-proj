// A 60-line test framework.
//
// Not doctest or Catch2, because the solver has zero dependencies and the
// test binary should too — `cmake . && make && ./ppp_tests` has to work on a
// machine with nothing installed, including CI.

#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& Registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& Failures() {
    static int f = 0;
    return f;
}

inline std::string& CurrentCase() {
    static std::string name;
    return name;
}

inline void Fail(const char* file, int line, const std::string& msg) {
    ++Failures();
    std::printf("  FAIL %s\n    %s:%d: %s\n", CurrentCase().c_str(), file, line, msg.c_str());
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

inline bool Approx(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

inline int RunAll(const char* filter) {
    int run = 0;
    for (const auto& c : Registry()) {
        if (filter && *filter && std::string(c.name).find(filter) == std::string::npos) continue;
        CurrentCase() = c.name;
        const int before = Failures();
        c.fn();
        ++run;
        if (Failures() == before) std::printf("  ok   %s\n", c.name);
    }
    std::printf("\n%d case(s), %d failure(s)\n", run, Failures());
    return Failures() == 0 ? 0 : 1;
}

}  // namespace test

// __LINE__ needs a second expansion before pasting, or every case in a file
// tries to define the same symbol.
#define TEST_PASTE_(a, b) a##b
#define TEST_PASTE(a, b) TEST_PASTE_(a, b)

#define TEST_CASE(name)                                                               \
    static void TEST_PASTE(test_fn_, __LINE__)();                                     \
    static ::test::Registrar TEST_PASTE(test_reg_, __LINE__)(name,                    \
                                                             TEST_PASTE(test_fn_, __LINE__)); \
    static void TEST_PASTE(test_fn_, __LINE__)()

#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) ::test::Fail(__FILE__, __LINE__, "CHECK(" #expr ")");   \
    } while (0)

#define CHECK_MSG(expr, msg)                                                 \
    do {                                                                     \
        if (!(expr)) ::test::Fail(__FILE__, __LINE__, std::string(msg));     \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                             \
    do {                                                                                  \
        const double va = (a), vb = (b);                                                  \
        if (!::test::Approx(va, vb, eps))                                                 \
            ::test::Fail(__FILE__, __LINE__,                                              \
                         std::string(#a " ≈ " #b ": ") + std::to_string(va) + " vs " +    \
                             std::to_string(vb));                                         \
    } while (0)

#define CHECK_EQ(a, b)                                                                        \
    do {                                                                                      \
        const auto va = (a);                                                                  \
        const auto vb = (b);                                                                  \
        if (!(va == vb))                                                                      \
            ::test::Fail(__FILE__, __LINE__, std::string(#a " == " #b " failed"));            \
    } while (0)
