#include "taelia/sim/clouds.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "taelia/shared_constants.h"
#include "taelia/sim/clock.h"
#include "taelia/sim/hash.h"
#include "taelia/sim/noise.h"

namespace taelia {

namespace {

constexpr int kN = CloudWater::kN;
constexpr double kTexel = CloudWater::kTexel;
constexpr int kSlices = 16;           // the grid is updated a sixteenth at a time, once a second
constexpr double kNaturalTau = 7200;  // s: the cloud water follows the day's tendency
constexpr double kSetTau = 120;       // s: ... or the world settings, faster
constexpr double kRainDisc = 12000;   // m: --rain gathers cloud water this far round the camera

// Coverage from cloud water: none below 6, a closed sky from 30 on.
double coverOf(double w) { return std::clamp((w - 6.0) / 24.0, 0.0, 1.0); }

}  // namespace

double CloudWater::at(double u, double v) const {
    const double fu = u / kTexel - double(u0) - 0.5, fv = v / kTexel - double(v0) - 0.5;
    if (fu < 0.0 || fv < 0.0 || fu > kN - 1.0 || fv > kN - 1.0 || water.empty()) return -1.0;
    const int i = std::min(int(fu), kN - 2), j = std::min(int(fv), kN - 2);
    const double a = fu - i, b = fv - j;
    const auto at2 = [&](int x, int y) {
        const int sx = int(((u0 + x) % kN + kN) % kN), sy = int(((v0 + y) % kN + kN) % kN);
        return double(water[size_t(sy) * kN + size_t(sx)]);
    };
    return (at2(i, j) * (1 - a) + at2(i + 1, j) * a) * (1 - b) + (at2(i, j + 1) * (1 - a) + at2(i + 1, j + 1) * a) * b;
}

double CloudMap::cover(double seconds) const {
    if (override_ >= 0) return std::clamp(override_, 0.0, 1.0);
    // Weather drifts over days: mostly fair, sometimes overcast, sometimes clear.
    const double n = fbm3(seconds, 0.0, 0.0, 2.6 * WorldClock::secondsPerDay(), 3, 0.5, mix64(seed_ ^ 0xc10d));
    return std::clamp(0.36 + 0.9 * n, 0.04, 0.96);
}

// What the cloud water tends toward here: the day's cover varied by region, overcast days past the rain line.
double CloudMap::target(double u, double v) const {
    double c = cover(t_);
    if (override_ < 0) c = std::clamp(c + 0.18 * fbm2(u, v, 40000.0, 2, 0.5, mix64(seed_ ^ 0x3e9)), 0.0, 1.0);
    double w = 6.0 + 24.0 * c * (1.0 + 4.0 * std::max(c - 0.75, 0.0));  // coverage(target) = c up to 0.75
    if (rain_ > 0.0) {
        const double du = u - focusU_, dv = v - focusV_;
        if (du * du + dv * dv < kRainDisc * kRainDisc) w = std::max(w, double(k::WEATHER_RAIN) + 25.0 * std::min(rain_, 1.0));
    }
    return w;
}

void CloudMap::fill(int i, int j) {
    const int64_t U = u0_ + i, V = v0_ + j;
    const int si = slot(U), sj = slot(V);
    w(si, sj) = float(target((double(U) + 0.5) * kTexel, (double(V) + 0.5) * kTexel));
    charge_[size_t(sj) * kN + size_t(si)] = 0.0f;
}

void CloudMap::place(double u, double v) {
    const int64_t cu = int64_t(std::floor(u / kTexel)), cv = int64_t(std::floor(v / kTexel));
    const int64_t wantU = cu - kN / 2, wantV = cv - kN / 2;
    if (!placed_) {
        water_.assign(size_t(kN) * kN, 0.0f);
        charge_.assign(size_t(kN) * kN, 0.0f);
        u0_ = wantU;
        v0_ = wantV;
        placed_ = true;
        for (int j = 0; j < kN; ++j)
            for (int i = 0; i < kN; ++i) fill(i, j);
        return;
    }
    if (std::abs(cu - (u0_ + kN / 2)) <= 8 && std::abs(cv - (v0_ + kN / 2)) <= 8) return;
    const int64_t oldU = u0_, oldV = v0_;
    u0_ = wantU;
    v0_ = wantV;
    for (int j = 0; j < kN; ++j)
        for (int i = 0; i < kN; ++i) {
            const int64_t U = u0_ + i, V = v0_ + j;
            if (U < oldU || U >= oldU + kN || V < oldV || V >= oldV + kN) fill(i, j);
        }
}

void CloudMap::setFocus(double x, double z) {
    focusU_ = x - disp_.x;
    focusV_ = z - disp_.z;
}

void CloudMap::step(double dt, const dvec3& wind, double seconds) {
    disp_.x += wind.x * dt;
    disp_.z += wind.z * dt;
    t_ = seconds;
    place(focusU_, focusV_);
    if (snapshot_.load() == nullptr) acc_ = 1.0;  // publish at once
    acc_ += dt;
    if (acc_ < 1.0) return;
    acc_ -= 1.0;
    // Once a second, a sixteenth of the grid: each texel sees 16 s pass.
    const double span = double(kSlices);
    const double tau = (override_ >= 0 || rain_ > 0) ? kSetTau : kNaturalTau, keep = std::exp(-span / tau);
    const int slice = int(updates_ % kSlices);
    ++updates_;
    for (int j = slice; j < kN; j += kSlices)
        for (int i = 0; i < kN; ++i) {
            const int64_t U = u0_ + i, V = v0_ + j;
            const int si = slot(U), sj = slot(V);
            const double t = target((double(U) + 0.5) * kTexel, (double(V) + 0.5) * kTexel);
            double water = t + (double(w(si, sj)) - t) * keep;
            // Rain takes from its cloud.
            const double rain = std::clamp((water - k::WEATHER_RAIN) / 20.0, 0.0, 1.0);
            water = std::max(water - rain * k::WEATHER_RAIN_TAKE * span, 0.0);
            w(si, sj) = float(water);
            // A storm where clouds run into each other past the storm line: friction there charges lightning.
            if (water > k::WEATHER_STORM && i > 0 && j > 0 && i < kN - 1 && j < kN - 1) {
                const double gx = (w(slot(U + 1), sj) - w(slot(U - 1), sj)) / (2.0 * kTexel);
                const double gz = (w(si, slot(V + 1)) - w(si, slot(V - 1))) / (2.0 * kTexel);
                const double front = std::clamp(std::hypot(gx, gz) / k::WEATHER_FRONT, 0.0, 1.0);
                float& q = charge_[size_t(sj) * kN + size_t(si)];
                q += float((water - k::WEATHER_STORM) * front * k::WEATHER_CHARGE * span);
                if (q > k::WEATHER_STRIKE) {
                    q -= float(k::WEATHER_STRIKE);
                    const uint64_t h = hashCombine(hashCombine(seed_, uint64_t(U) * 131 + uint64_t(V)), uint64_t(updates_));
                    const double u = (double(U) + hashToUnit(mix64(h))) * kTexel, v = (double(V) + hashToUnit(mix64(h ^ 9))) * kTexel;
                    strikes_.push_back({u + disp_.x, 0.0, v + disp_.z});
                }
            }
        }
    publish();
}

void CloudMap::publish() {
    auto snap = std::make_shared<CloudWater>();
    snap->u0 = u0_;
    snap->v0 = v0_;
    snap->water = water_;
    snapshot_.store(std::move(snap));
}

void CloudMap::setOverride(double cover) {
    if (cover == override_) return;
    override_ = cover;
    if (!placed_) return;
    for (int j = 0; j < kN; ++j)
        for (int i = 0; i < kN; ++i) fill(i, j);
    publish();
}

double CloudMap::coverageAt(double u, double v, double seconds, const CloudWater* water) const {
    // The third axis is time: the pattern changes over hours while the wind carries it.
    const double n = fbm3(u, v, seconds * 0.4, 9000.0, 5, 0.5, mix64(seed_ ^ 0xc1a0));
    const double c = n * 1.4 + 0.5;
    const double cw = water ? water->at(u, v) : -1.0;
    const double local = cw >= 0.0 ? coverOf(cw) : coverOf(target(u, v));  // past the grid: the tendency
    const double th = 1.0 - local * 1.05;
    return smoothstep(th - 0.12, th + 0.18, c);
}

double CloudMap::coverageAt(double u, double v, double seconds) const {
    const std::shared_ptr<const CloudWater> snap = snapshot_.load();
    return coverageAt(u, v, seconds, snap.get());
}

double CloudMap::baseAt(double u, double v) const {
    return 1500.0 + 450.0 * fbm3(u, v, 0.0, 30000.0, 2, 0.5, mix64(seed_ ^ 0xba5e));
}

double CloudMap::thicknessFor(double coverage) const { return 350.0 + 2300.0 * std::pow(coverage, 1.6); }

CloudSample CloudMap::sample(double x, double z) const {
    const double u = x - disp_.x, v = z - disp_.z;
    CloudSample s;
    s.coverage = coverageAt(u, v, t_);
    s.base = baseAt(u, v);
    s.thickness = thicknessFor(s.coverage);
    return s;
}

double CloudMap::waterAt(double x, double z) const {
    const double u = x - disp_.x, v = z - disp_.z;
    const std::shared_ptr<const CloudWater> snap = snapshot_.load();
    const double cw = snap ? snap->at(u, v) : -1.0;
    return cw >= 0.0 ? cw : target(u, v);
}

double CloudMap::rainAt(double x, double z) const {
    return std::clamp((waterAt(x, z) - k::WEATHER_RAIN) / 20.0, 0.0, 1.0);
}

double CloudMap::stormAt(double x, double z) const {
    const double water = waterAt(x, z);
    if (water <= k::WEATHER_STORM) return 0.0;
    const double gx = (waterAt(x + kTexel, z) - waterAt(x - kTexel, z)) / (2.0 * kTexel);
    const double gz = (waterAt(x, z + kTexel) - waterAt(x, z - kTexel)) / (2.0 * kTexel);
    return std::clamp((water - k::WEATHER_STORM) / 10.0, 0.0, 1.0) * std::clamp(std::hypot(gx, gz) / k::WEATHER_FRONT, 0.0, 1.0);
}

std::vector<dvec3> CloudMap::takeStrikes() {
    std::vector<dvec3> out;
    out.swap(strikes_);
    return out;
}

uint64_t CloudMap::stateHash() const {
    uint64_t h = hashCombine(uint64_t(u0_), uint64_t(v0_));
    h = hashCombine(h, uint64_t(updates_));
    for (const float x : water_) h = hashCombine(h, std::bit_cast<uint32_t>(x));
    for (const float x : charge_) h = hashCombine(h, std::bit_cast<uint32_t>(x));
    return h;
}

}  // namespace taelia
