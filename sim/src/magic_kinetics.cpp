#include <algorithm>
#include "taelia/sim/magic_kinetics.h"

#include <cmath>

#include "taelia/shared_constants.h"
#include "taelia/sim/fmath.h"
#include "taelia/sim/math.h"

namespace taelia {

namespace {

constexpr float kMassMu[kAspects] = {k::MAGIC_MASS_EARTH, k::MAGIC_MASS_WATER,     k::MAGIC_MASS_NATURE, k::MAGIC_MASS_FIRE,
                                     k::MAGIC_MASS_AIR,   k::MAGIC_MASS_LIGHTNING, k::MAGIC_MASS_LIGHT,  k::MAGIC_MASS_NEUTRAL};
constexpr float kShow[kAspects] = {k::MAGIC_SHOW_EARTH, k::MAGIC_SHOW_WATER,     k::MAGIC_SHOW_NATURE, k::MAGIC_SHOW_FIRE,
                                   k::MAGIC_SHOW_AIR,   k::MAGIC_SHOW_LIGHTNING, k::MAGIC_SHOW_LIGHT,  0.0f};

}  // namespace

const char* aspectName(Aspect a) {
    static const char* const names[kAspects] = {"earth", "water", "nature", "fire", "air", "lightning", "light", "neutral"};
    return int(a) < kAspects ? names[int(a)] : "?";
}

double MagicDensities::total() const {
    double s = 0;
    for (float r : rho) s += r;
    return s;
}

namespace magic {

double moteMass(Aspect a) { return double(kMassMu[int(a)]) * k::MAGIC_MU; }
double theta0() { return k::MAGIC_THETA0; }
double showThreshold(Aspect a) { return kShow[int(a)]; }

double moteDiameter(Aspect a) {
    // n b = 1/3 at the threshold: (rho*/m)(2/3 pi d^3) = 1/3, so d^3 = m / (2 pi rho*). Neutral is air's size.
    const Aspect s = a == Aspect::Neutral ? Aspect::Air : a;
    return fmath::cbrt(moteMass(s) / (2.0 * kPi * showThreshold(s)));
}

double crossSection(Aspect a) {
    const double d = moteDiameter(a);
    return kPi * d * d;
}

double excludedVolume(Aspect a) {
    const double d = moteDiameter(a);
    return 2.0 / 3.0 * kPi * d * d * d;
}

double moteSpeed(Aspect a, double theta) { return std::sqrt(3.0 * theta / moteMass(a)); }
double soundSpeed(Aspect a, double theta) { return std::sqrt(double(k::MAGIC_GAMMA) * theta / moteMass(a)); }
double numberDensity(Aspect a, double rho) { return rho / moteMass(a); }
double partialPressure(Aspect a, double rho, double theta) { return rho * theta / moteMass(a); }

double meanFreePath(Aspect a, double nTot, double theta) {
    const double v = moteSpeed(a, theta);
    return 1.0 / (std::sqrt(2.0) * crossSection(a) * nTot + v / (3.0 * double(k::MAGIC_D_MAX)));
}

double diffusivity(Aspect a, double nTot, double theta) { return moteSpeed(a, theta) * meanFreePath(a, nTot, theta) / 3.0; }

double totalNumberDensity(const MagicDensities& d) {
    double n = 0;
    for (int a = 0; a < kAspects; ++a) n += numberDensity(Aspect(a), d.rho[a]);
    return n;
}

double totalPressure(const MagicDensities& d, double theta) { return totalNumberDensity(d) * theta; }

double mixtureSoundSpeed(const MagicDensities& d, double theta) {
    const double rho = d.total();
    return rho > 0 ? std::sqrt(double(k::MAGIC_GAMMA) * totalPressure(d, theta) / rho) : 0.0;
}

double dynamicViscosity(const MagicDensities& d, double theta) {
    const double n = totalNumberDensity(d);
    double mu = 0;
    for (int a = 0; a < kAspects; ++a) mu += d.rho[a] * diffusivity(Aspect(a), n, theta);
    return mu;
}

double kinematicViscosity(const MagicDensities& d, double theta) {
    const double rho = d.total();
    return rho > 0 ? dynamicViscosity(d, theta) / rho : 0.0;
}

double inertiaTau(Aspect a) {
    return double(k::MAGIC_TAU_INERTIA_AIR) * std::sqrt(double(kMassMu[int(a)]) / double(k::MAGIC_MASS_AIR));
}

double effusionFlux(Aspect a, double pressure, double theta) {
    return pressure / std::sqrt(2.0 * kPi * moteMass(a) * theta);
}

// A hair under the exact value, so the threshold density itself converts despite rounding in m and Theta.
double fireToLightningPressure() { return partialPressure(Aspect::Fire, k::MAGIC_FIRE_TO_LIGHTNING, k::MAGIC_THETA0) * (1.0 - 1e-12); }
double toLightPressure() { return partialPressure(Aspect::Fire, k::MAGIC_FIRE_TO_LIGHT, k::MAGIC_THETA0) * (1.0 - 1e-12); }

Conversion fireConversion(double rhoFire, double theta) {
    const double p = partialPressure(Aspect::Fire, rhoFire, theta);
    if (p >= toLightPressure()) return Conversion::ToLight;
    if (p >= fireToLightningPressure()) return Conversion::ToLightning;
    return Conversion::None;
}

Conversion lightningConversion(double rhoLightning, double theta) {
    return partialPressure(Aspect::Lightning, rhoLightning, theta) >= toLightPressure() ? Conversion::ToLight : Conversion::None;
}

double effectFactor(double rhoTotal) {
    const double e = rhoTotal - double(k::MAGIC_EFFECT_THRESHOLD);
    return e > 0 ? e : 0.0;
}

bool visible(double rhoTotal) { return rhoTotal > double(k::MAGIC_DEW_POINT); }

double heldAfter(double held, double around, double dt) {
    const double target = std::min(double(k::MAGIC_HELD), std::max(around, 0.0));
    return target + (held - target) * fmath::exp(-dt / double(k::MAGIC_HELD_TAU));
}

bool fallsApart(double held) { return held < double(k::MAGIC_FALL_APART) * k::MAGIC_HELD; }

int fireflyCount(double light) {
    if (light < k::MAGIC_SHOW_LIGHT) return 0;
    return 3 + int(std::floor((light - k::MAGIC_SHOW_LIGHT) / 3.0 + 1e-9));
}

void fireflyGroups(double light, int& big, int& small) {
    const int n = fireflyCount(light);
    big = n / k::MAGIC_FIREFLY_MERGE;
    small = n % k::MAGIC_FIREFLY_MERGE;
}

}  // namespace magic

}  // namespace taelia
