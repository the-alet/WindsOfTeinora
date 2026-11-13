// A minimal test harness: TEST(name) { CHECK(...); }. Run all with taelia_tests, or one with taelia_tests <name>.
#pragma once
#include <cmath>
#include <cstdio>
#include <vector>

namespace taelia::test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

inline int& failures() {
    static int f = 0;
    return f;
}

struct Reg {
    Reg(const char* n, void (*f)()) { registry().push_back({n, f}); }
};

inline void report(const char* file, int line, const char* what) {
    ++failures();
    std::printf("  FAILED %s:%d: %s\n", file, line, what);
}

}  // namespace taelia::test

#define TEST(name)                                                    \
    static void name();                                               \
    static ::taelia::test::Reg reg_##name(#name, name);               \
    static void name()

#define CHECK(cond)                                                   \
    do {                                                              \
        if (!(cond)) ::taelia::test::report(__FILE__, __LINE__, #cond); \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                  \
    do {                                                                                       \
        const double va_ = double(a), vb_ = double(b);                                         \
        if (!(std::fabs(va_ - vb_) <= double(eps))) {                                          \
            char msg_[256];                                                                    \
            std::snprintf(msg_, sizeof msg_, "%s = %.9g, %s = %.9g", #a, va_, #b, vb_);         \
            ::taelia::test::report(__FILE__, __LINE__, msg_);                                  \
        }                                                                                      \
    } while (0)
