#include "taelia/sim/wind.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "taelia/shared_constants.h"
#include "taelia/sim/hash.h"
#include "taelia/sim/noise.h"

namespace taelia {

namespace {

constexpr double kRoughness = 0.05;   // m: grass and low plants slow the air near the ground
constexpr double kIntensity = 0.28;   // eddy strength (standard deviation) relative to the local mean wind
constexpr double kCalm = 0.22;        // m/s of stirring even in still air
constexpr double kEddy = 18.0;        // m: the largest eddies near the ground
constexpr double kGustFront = 140.0;  // m: gusts sweep across the land this wide
constexpr double kSigma2 = 0.202;     // measured standard deviation of fbm3 with 2 octaves (gain 0.5)
constexpr double kSigma3 = 0.177;     // ... and with 3

}  // namespace

dvec3 WindField::meanAt(double aboveGround) const {
    const double z = std::max(aboveGround, kRoughness * 1.5);
    const double f = std::log(z / kRoughness) / std::log(double(k::WIND_REFERENCE_HEIGHT) / kRoughness);
    return mean_ * clampd(f, 0.0, 1.5);
}

void WindField::step(double dt) {
    carried_ += mean_ * dt;
    evolved_ += dt * (0.35 + 0.08 * length(mean_));
}

uint64_t WindField::stateHash() const {
    uint64_t h = seed_;
    for (const double v : {carried_.x, carried_.y, carried_.z, evolved_, mean_.x, mean_.y, mean_.z})
        h = hashCombine(h, std::bit_cast<uint64_t>(v));
    return h;
}

dvec3 WindField::at(const dvec3& p, double aboveGround) const {
    const dvec3 m = meanAt(aboveGround);
    // Frozen turbulence: the pattern rides the set wind (the same at every height) and slowly changes as it goes.
    const dvec3 q = p - carried_;
    // Gust fronts: the mean itself swells and fades as broad patches pass.
    const double gust = fbm3(q.x, evolved_ * 0.25, q.z, kGustFront, 2, 0.5, hashCombine(seed_, 0x9057)) / kSigma2;
    // Eddies: three channels of smooth noise; vertical ones fade toward the ground, which blocks them.
    const double sigma = kIntensity * length(m) + kCalm;
    const double y = q.y * 1.5 + evolved_;
    const double ex = fbm3(q.x, y, q.z, kEddy, 3, 0.5, hashCombine(seed_, 0xed1)) / kSigma3;
    const double ey = fbm3(q.x, y, q.z, kEddy, 3, 0.5, hashCombine(seed_, 0xed2)) / kSigma3;
    const double ez = fbm3(q.x, y, q.z, kEddy, 3, 0.5, hashCombine(seed_, 0xed3)) / kSigma3;
    const double vertical = 0.5 * clampd(aboveGround / 3.0, 0.0, 1.0);
    return m * (1.0 + 0.25 * clampd(gust, -2.0, 2.0)) + dvec3{ex, ey * vertical, ez} * sigma;
}

}  // namespace taelia
