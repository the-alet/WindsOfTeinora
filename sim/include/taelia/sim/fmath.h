// Own transcendental functions for the simulation: plain IEEE arithmetic (no library calls whose last bits differ
// between toolchains), so the magic field gives the same bits under MSVC here and under Unreal's toolchain later.
// sqrt, floor and ldexp are exact in IEEE and stay from <cmath>.
#pragma once
#include <cmath>

namespace taelia::fmath {

// e^x: x = k ln2 + r with |r| <= ln2/2, then a degree-13 Taylor polynomial in r (error below 1e-16 relative).
inline double exp(double x) {
    if (x < -700.0) return 0.0;
    if (x > 700.0) x = 700.0;
    constexpr double kInvLn2 = 1.4426950408889634;
    constexpr double kLn2Hi = 6.93147180369123816490e-01;
    constexpr double kLn2Lo = 1.90821492927058770002e-10;
    const double k = std::floor(x * kInvLn2 + 0.5);
    const double r = (x - k * kLn2Hi) - k * kLn2Lo;
    double p = 1.0 / 6227020800.0;  // 1/13!
    constexpr double kInvFact[13] = {1.0 / 479001600.0, 1.0 / 39916800.0, 1.0 / 3628800.0, 1.0 / 362880.0, 1.0 / 40320.0,
                                     1.0 / 5040.0,      1.0 / 720.0,      1.0 / 120.0,      1.0 / 24.0,      1.0 / 6.0,
                                     0.5,               1.0,              1.0};
    for (double c : kInvFact) p = p * r + c;
    return std::ldexp(p, int(k));
}

// Cube root by Newton steps from a bit-exact start (x > 0).
inline double cbrt(double x) {
    if (x <= 0.0) return 0.0;
    int e = 0;
    const double m = std::frexp(x, &e);  // x = m 2^e, m in [0.5, 1)
    const int q = (e >= 0 ? e : e - 2) / 3, rem = e - 3 * q;
    double y = std::ldexp(0.7 + 0.3 * m, q) * (rem == 0 ? 1.0 : (rem == 1 ? 1.2599210498948732 : 1.5874010519681994));
    for (int i = 0; i < 6; ++i) y = y - (y * y * y - x) / (3.0 * y * y);
    return y;
}

}  // namespace taelia::fmath
