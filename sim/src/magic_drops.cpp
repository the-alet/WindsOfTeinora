// Light drops (step 5e): single fast motes the suns shoot (and the planet sends back a little of), walked through the
// finest level along their way. Each passing cell may stop one (its mean free path shortens in denser magic). Where
// life magic rules the cell absorbs it and grows; anywhere else it leaves a speck of light magic and stirs a small
// eddy. The ground swallows the rest. Deterministic: the drops of a tick come from integer hashes.
#include <algorithm>
#include <cmath>

#include "magic_internal.h"
#include "taelia/sim/fmath.h"
#include "taelia/sim/hash.h"

namespace taelia {

using namespace mf;

void MagicField::Impl::drops(int l, double dtTick, int64_t tick) {
    Level& L = lv[l];
    if (dropSources.empty()) return;
    const double nRef = double(k::MAGIC_AMBIENT_FREE) / mass[int(Aspect::Air)];
    const double top = double(L.ny) * L.dy;
    for (size_t src = 0; src < dropSources.size(); ++src) {
        const DropSource& s = dropSources[src];
        if (s.rate <= 0.0 || s.direction.y <= 0.02) continue;
        // Whole drops this tick (the fraction carries over from tick to tick, exactly).
        const int64_t count = int64_t(std::floor(s.rate * dtTick * double(tick + 1))) - int64_t(std::floor(s.rate * dtTick * double(tick)));
        const dvec3 d = normalize(s.direction) * -1.0;  // the way it travels
        for (int64_t i = 0; i < count; ++i) {
            const uint64_t h = hashCombine(hashCombine(hashCombine(opt.seed, uint64_t(tick)), src), uint64_t(i));
            // Enter over a random column of the ring, from above the level's top.
            const double fx = hashToUnit(mix64(h)), fz = hashToUnit(mix64(h ^ 0x51ed));
            const int64_t X = L.ox + int64_t(fx * L.nc), Z = L.oz + int64_t(fz * L.nc);
            const int col = L.slotColumn(X, Z);
            dvec3 p{(double(X) + 0.5) * L.dx, (double(L.kb[size_t(col)]) * L.dy + top - 0.5 * L.dy), (double(Z) + 0.5) * L.dx};
            const double step = 0.5 * std::min(L.dx, L.dy);
            uint64_t r = mix64(h ^ 0xd209);
            for (int n = 0; n < 4096; ++n) {
                p += d * step;
                const int64_t cx = int64_t(std::floor(p.x / L.dx)), cz = int64_t(std::floor(p.z / L.dx)), cy = int64_t(std::floor(p.y / L.dy));
                const int c = L.find(cx, cz, cy);
                if (c == -1 || (c == -3 && n > 4 * L.ny)) break;  // left the ring (or never came down into it)
                if (c == -2) break;                             // the ground swallows it
                if (c < 0) continue;                            // still above the column's top
                double nTot = 0, rhoTot = 0;
                for (int a = 0; a < kA; ++a) {
                    nTot += L.st[L.at(a, c)] / mass[a];
                    rhoTot += L.st[L.at(a, c)];
                }
                const double lambda = double(k::MAGIC_DROP_PATH) * nRef / std::max(nTot, 1e-6);
                r = mix64(r + 0x9e37);
                if (hashToUnit(r) >= 1.0 - fmath::exp(-step / lambda)) continue;
                // Stopped here.
                const float life = L.st[L.at(int(Aspect::Life), c)];
                if (life > 0.4 * rhoTot) {
                    const float grow = std::min(float(k::MAGIC_DROP_GROWTH), L.st[L.at(int(Aspect::Neutral), c)]);
                    L.st[L.at(int(Aspect::Life), c)] += grow;
                    L.st[L.at(int(Aspect::Neutral), c)] -= grow;
                } else {
                    L.st[L.at(int(Aspect::Light), c)] += float(k::MAGIC_DROP_LIGHT);
                    // A small vortex around its path in the cells about it (zero net push).
                    const int64_t K = cy;
                    for (int64_t oz = -1; oz <= 1; ++oz)
                        for (int64_t oy = -1; oy <= 1; ++oy)
                            for (int64_t ox = -1; ox <= 1; ++ox) {
                                const int e = L.find(cx + ox, cz + oz, K + oy);
                                if (e < 0) continue;
                                const dvec3 off{double(ox), double(oy), double(oz)};
                                const dvec3 swirl = cross(d, off) * (double(k::MAGIC_DROP_KICK) / (1.0 + dot(off, off)));
                                L.st[L.at(kU, e)] += float(swirl.x);
                                L.st[L.at(kU + 1, e)] += float(swirl.y);
                                L.st[L.at(kU + 2, e)] += float(swirl.z);
                            }
                }
                L.deviation[size_t(L.brickOf(c))] = 1e9f;  // it steps now
                break;
            }
        }
    }
}

// Raindrops carry water magic down: a few land in random columns round the camera each tick (a light rain, a few).
void MagicField::Impl::raindrops(int l, double dtTick, int64_t tick) {
    if (rain <= 0.0) return;
    Level& L = lv[l];
    const double rate = rain * double(k::MAGIC_RAIN_DROPS);
    const int64_t count = int64_t(std::floor(rate * dtTick * double(tick + 1))) - int64_t(std::floor(rate * dtTick * double(tick)));
    for (int64_t i = 0; i < count; ++i) {
        const uint64_t h = hashCombine(hashCombine(opt.seed ^ 0x7a1, uint64_t(tick)), uint64_t(i));
        const int64_t X = L.ox + int64_t(hashToUnit(mix64(h)) * L.nc), Z = L.oz + int64_t(hashToUnit(mix64(h ^ 0x3b)) * L.nc);
        const int c = L.find(X, Z, L.kb[size_t(L.slotColumn(X, Z))]);
        if (c < 0) continue;
        L.st[L.at(int(Aspect::Water), c)] += float(k::MAGIC_RAIN_WATER);
        L.deviation[size_t(L.brickOf(c))] = 1e9f;
    }
}

}  // namespace taelia
