// Manifestations (step 5f), after each tick of the finest level, serially in cell order (deterministic): earth and
// life condensate past MAGIC_SPAWN_KG grows a mineral or a sprout that stays (it holds its own magic like any object;
// the rest goes back to the gas); light condensate shows as fireflies (3 at 40, one more for every 3 past it; ten merge
// into a big one that sends out waves of light magic); air condensate spins a whirlwind; fire condensate burns.
#include <algorithm>
#include <cmath>

#include "magic_internal.h"
#include "taelia/sim/hash.h"

namespace taelia {

using namespace mf;

void MagicField::Impl::manifest(int l, int64_t tick) {
    Level& L = lv[l];
    fireflies.clear();
    flames.clear();
    hotSpots.clear();
    dense = false;
    if (L.cond.empty()) return;
    const double volume = L.dx * L.dx * L.dy;
    const double dtTick = double(L.every) / double(k::SIM_STEP_HZ);
    const int64_t waveTicks = std::max<int64_t>(1, int64_t(std::lround(k::MAGIC_FIREFLY_WAVE_EVERY / dtTick)));
    const auto condOf = [&](int m, int c) -> float& { return L.cond[size_t(m) * L.cells + size_t(c)]; };
    for (int c = 0; c < L.cells; ++c) {
        const int s = c % L.cols, k = c / L.cols;
        const dvec3 centre{(double(L.worldX(s % L.nc)) + 0.5) * L.dx, (double(L.kb[size_t(s)] + k) + 0.5) * L.dy,
                           (double(L.worldZ(s / L.nc)) + 0.5) * L.dx};
        // Past the dew point it shows (and bends light): grow the box around it.
        {
            float total = 0;
            for (int a = 0; a < kA; ++a) total += L.st[L.at(a, c)];
            for (int m = 0; m < 6; ++m) total += condOf(m, c);
            if (total > float(k::MAGIC_DEW_POINT)) {
                const dvec3 half{0.5 * L.dx, 0.5 * L.dy, 0.5 * L.dx};
                if (!dense) denseLo = centre - half, denseHi = centre + half, dense = true;
                denseLo = {std::min(denseLo.x, centre.x - half.x), std::min(denseLo.y, centre.y - half.y), std::min(denseLo.z, centre.z - half.z)};
                denseHi = {std::max(denseHi.x, centre.x + half.x), std::max(denseHi.y, centre.y + half.y), std::max(denseHi.z, centre.z + half.z)};
            }
        }
        // Minerals and sprouts: they stay.
        for (const auto [m, kind, aspect, vol] : {std::tuple{0, ManifestKind::Mineral, Aspect::Earth, double(k::MAGIC_MINERAL_VOLUME)},
                                                  std::tuple{2, ManifestKind::Sprout, Aspect::Life, double(k::MAGIC_SPROUT_VOLUME)}}) {
            float& cond = condOf(m, c);
            if (cond * volume < k::MAGIC_SPAWN_KG) continue;
            const uint64_t h = hashCombine(hashCombine(opt.seed, uint64_t(c)), uint64_t(tick) * 3 + uint64_t(m));
            Manifestation o;
            o.id = nextManifest++;
            o.kind = kind;
            o.position = centre + dvec3{(hashToUnit(mix64(h)) - 0.5) * L.dx * 0.6, -0.5 * L.dy, (hashToUnit(mix64(h ^ 7)) - 0.5) * L.dx * 0.6};
            o.volume = float(vol);
            o.held = float(k::MAGIC_HELD);
            o.seed = uint32_t(h);
            manifestations.push_back(o);
            // It takes what it holds; the rest of the spawn mass goes back to the gas.
            cond -= float(k::MAGIC_SPAWN_KG / volume);
            L.st[L.at(int(aspect), c)] += float((k::MAGIC_SPAWN_KG - k::MAGIC_HELD * vol) / volume);
            L.deviation[size_t(L.brickOf(c))] = 1e9f;
        }
        // Fireflies are the condensed light: as many as SPEC counts for the density it stands for (40 and its excess);
        // every ten merge into a big one.
        const float light = condOf(5, c) > 0.0f ? float(k::MAGIC_SHOW_LIGHT) + condOf(5, c) : 0.0f;
        if (light >= float(k::MAGIC_SHOW_LIGHT)) {
            int big = 0, small = 0;
            magic::fireflyGroups(light, big, small);
            for (int i = 0; i < big + small; ++i) {
                const uint64_t h = hashCombine(hashCombine(opt.seed, uint64_t(c)), uint64_t(i) + 0x11ff);
                Firefly f;
                f.position = centre + dvec3{(hashToUnit(mix64(h)) - 0.5) * L.dx, (hashToUnit(mix64(h ^ 3)) - 0.5) * L.dy,
                                            (hashToUnit(mix64(h ^ 5)) - 0.5) * L.dx};
                f.big = i < big;
                f.seed = uint32_t(h);
                fireflies.push_back(f);
            }
            // A big one sends a wave of light magic out to the cells around it, from its own condensate.
            if (big > 0 && tick % waveTicks == 0) {
                for (int d = 0; d < 6; ++d) {
                    const int32_t n = L.nb[size_t(d) * L.cells + size_t(c)];
                    if (n < 0) continue;
                    const float give = std::min(float(k::MAGIC_FIREFLY_WAVE) * float(big), condOf(5, c) / 6.0f);
                    condOf(5, c) -= give;
                    L.st[L.at(int(Aspect::Light), n)] += give;
                    L.deviation[size_t(L.brickOf(n))] = 1e9f;
                }
            }
        }
        // A whirlwind: air condensate spins the cells about it (around the up axis).
        const float air = condOf(4, c);
        if (air > 0.0f) {
            const int sx = s % L.nc, sz = s / L.nc;
            for (int dz = -2; dz <= 2; ++dz)
                for (int dx = -2; dx <= 2; ++dx) {
                    if (dx == 0 && dz == 0) continue;
                    const int e = L.find(L.worldX(sx) + dx, L.worldZ(sz) + dz, int64_t(L.kb[size_t(s)] + k));
                    if (e < 0) continue;
                    const double r2 = double(dx * dx + dz * dz), swirl = k::MAGIC_WHIRL * air / r2;
                    L.st[L.at(kU, e)] += float(-dz * swirl);
                    L.st[L.at(kU + 2, e)] += float(dx * swirl);
                    L.deviation[size_t(L.brickOf(e))] = 1e9f;
                }
        }
        // Fire: flames where fire condensed (the campfire's own flame is drawn from its heat).
        const float fire = condOf(3, c);
        if (fire > 0.5f) flames.push_back({centre - dvec3{0, 0.5 * L.dy, 0}, double(fire)});
        // Past the dew point fire magic makes steam over water and sets plants burning.
        const float hot = L.st[L.at(int(Aspect::Fire), c)] + fire;
        if (hot > float(k::MAGIC_FIRE_STEAM)) {
            const MagicColumn& col = L.column[size_t(s)];
            const double y0 = double(L.kb[size_t(s)] + k) * L.dy;
            const bool water = col.underwater() && col.water >= y0 && col.water < y0 + L.dy + 1.0;
            hotSpots.push_back({water ? dvec3{centre.x, col.water, centre.z} : centre, double(hot), water});
        }
    }
    std::sort(flames.begin(), flames.end(), [](const Flame& a, const Flame& b) { return a.strength > b.strength; });
    if (flames.size() > 3) flames.resize(3);
}

}  // namespace taelia
