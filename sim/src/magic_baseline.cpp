#include "taelia/sim/magic_baseline.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "taelia/shared_constants.h"
#include "taelia/sim/fmath.h"
#include "taelia/sim/plants.h"
#include "taelia/sim/terrain.h"

namespace taelia {

namespace {

// Average of f(y) = exp(-(y - ref)/L) over [a, b], with f = 1 below ref.
double avgExp(double a, double b, double ref, double L) {
    if (b - a < 1e-9) return a <= ref ? 1.0 : fmath::exp(-(a - ref) / L);
    double sum = 0;
    if (a < ref) sum += std::min(b, ref) - a;
    const double lo = std::max(a, ref);
    if (b > lo) sum += L * (fmath::exp(-(lo - ref) / L) - fmath::exp(-(b - ref) / L));
    return sum / (b - a);
}

double freeMagic(double y) { return 1.0 - smoothstep(k::MAGIC_FREE_TOP0, k::MAGIC_FREE_TOP1, y); }

double earthFactor(const MagicColumn& c) { return 0.9 + 0.3 * clampd(c.rockSand, 0.0, 1.0); }

double crownNature(const std::vector<MagicCrown>* crowns, double x, double y, double z) {
    if (!crowns) return 0;
    double best = 0;
    for (const MagicCrown& c : *crowns) {
        const double dx = (x - c.center.x) / c.radius, dz = (z - c.center.z) / c.radius, dy = (y - c.center.y) / c.halfHeight;
        const double q = dx * dx + dy * dy + dz * dz;
        if (q < 1.0) best = std::max(best, 1.0 - smoothstep(0.5, 1.0, q));
    }
    return best * k::MAGIC_NATURE_CROWN;
}

// Crown ellipsoids of the grown skeletons (per kind and variant, at scale 1), measured once from their leaves.
struct CrownShape {
    double centerY = 0, radius = 0, halfHeight = 0;
};

const CrownShape& crownShape(TreeKind kind, int variant) {
    static CrownShape shapes[int(TreeKind::Count) * kTreeVariants];
    static std::once_flag once;
    std::call_once(once, [] {
        for (int kd = 0; kd < int(TreeKind::Count); ++kd)
            for (int v = 0; v < kTreeVariants; ++v) {
                const TreeSkeleton t = growTree(TreeKind(kd), treeSkeletonSeed(TreeKind(kd), v));
                double lo = 1e30, hi = -1e30;
                for (const LeafCard& l : t.leaves) {
                    lo = std::min(lo, double(l.center.y));
                    hi = std::max(hi, double(l.center.y));
                }
                CrownShape& s = shapes[kd * kTreeVariants + v];
                if (hi < lo) lo = hi = 0.7 * t.height;
                s.centerY = 0.5 * (lo + hi);
                s.halfHeight = std::max(1.0, 0.5 * (hi - lo) + 0.5);
                s.radius = std::max(1.0, double(t.crownRadius) + 0.5);
            }
    });
    return shapes[int(kind) * kTreeVariants + variant];
}

}  // namespace

void MagicBaseline::pondsNear(double x, double z, double radius, std::vector<Pond>& out) const {
    out.clear();
    if (terrain_) terrain_->pondsNear(x, z, radius, out);
}

MagicColumn MagicBaseline::column(double x, double z, const std::vector<Pond>& ponds) const {
    MagicColumn c;
    if (!terrain_) {
        c.ground = flat_;
        return c;
    }
    const TerrainSample s = terrain_->sample(x, z);
    c.ground = s.height;
    c.water = s.waterLevel;
    c.rockSand = double(s.materials.rock) + double(s.materials.sand);
    c.meadow = s.materials.meadow;
    if (!c.underwater()) {
        for (const Pond& p : ponds) {
            const double dx = x - p.cx, dz = z - p.cz;
            const double d = std::sqrt(dx * dx + dz * dz);
            const double shore = p.shoreRadius(std::atan2(dz, dx));
            const double dh = std::max(0.0, d - shore);
            if (dh < c.shoreDistance) {
                c.shoreDistance = dh;
                c.shoreLevel = p.level;
            }
        }
    }
    return c;
}

void MagicBaseline::crownsInArea(double x0, double z0, double x1, double z1, std::vector<MagicCrown>& out) const {
    out.clear();
    if (!terrain_) return;
    std::vector<TreeInstance> trees;
    constexpr double kReach = 12.0;  // crowns reach out from trunks standing this far outside the area
    treesInArea(*terrain_, x0 - kReach, z0 - kReach, x1 + kReach, z1 + kReach, trees);
    for (const TreeInstance& t : trees) {
        const CrownShape& s = crownShape(t.kind, t.variant);
        MagicCrown c;
        c.center = {t.position.x, t.position.y + s.centerY * t.scale, t.position.z};
        c.radius = s.radius * t.scale;
        c.halfHeight = s.halfHeight * t.scale;
        if (c.center.x + c.radius < x0 || c.center.x - c.radius > x1 || c.center.z + c.radius < z0 || c.center.z - c.radius > z1)
            continue;
        out.push_back(c);
    }
}

MagicDensities MagicBaseline::point(const MagicColumn& c, double x, double y, double z, const std::vector<MagicCrown>* crowns) const {
    const double yy = std::max(y, c.ground);
    MagicDensities d;
    d[Aspect::Earth] = float(k::MAGIC_EARTH_GROUND * earthFactor(c) * fmath::exp(-(yy - c.ground) / k::MAGIC_EARTH_SCALE));
    double water = 0;
    if (c.underwater())
        water = yy <= c.water ? k::MAGIC_WATER_NEAR : k::MAGIC_WATER_NEAR * fmath::exp(-(yy - c.water) / k::MAGIC_WATER_SCALE);
    else if (c.shoreDistance < 1e29)
        water = k::MAGIC_WATER_NEAR * fmath::exp(-c.shoreDistance / k::MAGIC_WATER_SCALE) *
                fmath::exp(-std::max(0.0, yy - c.shoreLevel) / k::MAGIC_WATER_SCALE);
    d[Aspect::Water] = float(water);
    double nature = 0;
    if (yy >= c.surface())
        nature = k::MAGIC_NATURE_MEADOW * c.meadow * fmath::exp(-(yy - c.ground) / k::MAGIC_NATURE_SCALE);
    d[Aspect::Nature] = float(std::max(nature, crownNature(crowns, x, yy, z)));
    const double free = k::MAGIC_AMBIENT_FREE * freeMagic(yy);
    d[Aspect::Air] = float(0.5 * free);
    d[Aspect::Neutral] = float(0.5 * free);
    return d;
}

MagicDensities MagicBaseline::average(const MagicColumn& c, double x, double z, double y0, double y1,
                                      const std::vector<MagicCrown>* crowns) const {
    const double a = std::max(y0, c.ground), b = std::max(y1, a);
    MagicDensities d;
    d[Aspect::Earth] = float(k::MAGIC_EARTH_GROUND * earthFactor(c) * avgExp(a, b, c.ground, k::MAGIC_EARTH_SCALE));
    double water = 0;
    if (c.underwater())
        water = k::MAGIC_WATER_NEAR * avgExp(a, b, c.water, k::MAGIC_WATER_SCALE);  // 1 inside the water, then the falloff
    else if (c.shoreDistance < 1e29)
        water = k::MAGIC_WATER_NEAR * fmath::exp(-c.shoreDistance / k::MAGIC_WATER_SCALE) *
                avgExp(a, b, c.shoreLevel, k::MAGIC_WATER_SCALE);
    d[Aspect::Water] = float(water);
    double nature = 0;
    if (!c.underwater()) nature = k::MAGIC_NATURE_MEADOW * c.meadow * avgExp(a, b, c.ground, k::MAGIC_NATURE_SCALE);
    d[Aspect::Nature] = float(std::max(nature, crownNature(crowns, x, 0.5 * (a + b), z)));
    const double free = k::MAGIC_AMBIENT_FREE * freeMagic(0.5 * (a + b));
    d[Aspect::Air] = float(0.5 * free);
    d[Aspect::Neutral] = float(0.5 * free);
    return d;
}

MagicDensities MagicBaseline::at(const dvec3& p) const {
    std::vector<Pond> ponds;
    pondsNear(p.x, p.z, 30.0, ponds);
    std::vector<MagicCrown> crowns;
    crownsInArea(p.x - 1, p.z - 1, p.x + 1, p.z + 1, crowns);
    return point(column(p.x, p.z, ponds), p.x, p.y, p.z, &crowns);
}

}  // namespace taelia
