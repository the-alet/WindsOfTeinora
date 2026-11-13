#include "taelia/sim/plants.h"

#include <algorithm>
#include <cmath>

#include "taelia/sim/hash.h"
#include "taelia/sim/noise.h"
#include "taelia/sim/terrain.h"

namespace taelia {

namespace {

struct Rng {
    uint64_t s;
    double next() {
        s = mix64(s + 0x9e3779b97f4a7c15ull);
        return hashToUnit(s);
    }
    double range(double a, double b) { return a + (b - a) * next(); }
};

struct Species {
    double height[2];         // metres
    double trunkRadius;       // base radius per metre of height
    double crownStart;        // fraction of the height where branches begin
    int branches[2];          // main branches on the trunk
    double branchAngle[2];    // from the trunk, radians (low branches use the second value)
    double branchLength;      // per metre of height
    double gravity;           // + bends up, - droops
    int twigs[2];             // twigs per main branch
    double twigLength;        // per metre of parent
    double leafSize;          // card edge, metres
    int leavesPerTwig[2];
    bool cone;                // branch length shrinks toward the top (spire)
};

const Species kSpecies[] = {
    // Veyl: tall, slender, high narrow crown.
    {{14, 22}, 0.014, 0.42, {18, 24}, {0.45, 0.75}, 0.30, 0.55, {6, 9}, 0.45, 0.85, {9, 13}, false},
    // Ombrel: short trunk, broad spreading crown.
    {{9, 14}, 0.030, 0.30, {10, 14}, {0.75, 1.10}, 0.55, 0.25, {8, 12}, 0.48, 1.0, {10, 14}, false},
    // Sarn: a spire with drooping tiers.
    {{12, 20}, 0.018, 0.12, {32, 42}, {1.30, 1.45}, 0.36, -0.35, {6, 9}, 0.42, 0.8, {8, 11}, true},
};

dvec3 perpendicular(const dvec3& d) {
    const dvec3 a = std::fabs(d.y) < 0.9 ? dvec3{0, 1, 0} : dvec3{1, 0, 0};
    return normalize(cross(d, a));
}

dvec3 rotateAround(const dvec3& v, const dvec3& axis, double angle) {
    const double c = std::cos(angle), s = std::sin(angle);
    return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1 - c));
}

fvec3 f3(const dvec3& v) { return {float(v.x), float(v.y), float(v.z)}; }
dvec3 d3(const fvec3& v) { return {v.x, v.y, v.z}; }

struct Grower {
    const Species& sp;
    Rng rng;
    TreeSkeleton& tree;

    // A curving limb of `steps` segments; returns the index of its last segment and fills its points.
    int limb(dvec3 p, dvec3 dir, double length, double r0, double r1, int parent, uint8_t order, double gravity,
             std::vector<dvec3>* points) {
        const int steps = std::max(2, int(length / (order == 0 ? 1.2 : 0.7)));
        const double step = length / steps;
        if (points) points->push_back(p);
        for (int i = 0; i < steps; ++i) {
            dir = normalize(dir + dvec3{0, gravity * 0.18, 0} +
                            dvec3{rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)} * (order == 0 ? 0.04 : 0.12));
            if (p.y + dir.y * step < 0.3) dir = normalize(dvec3{dir.x, std::max(dir.y, 0.1), dir.z});  // limbs turn up before the ground
            const dvec3 q = p + dir * step;
            TreeSegment s;
            s.a = f3(p);
            s.b = f3(q);
            s.ra = float(r0 + (r1 - r0) * double(i) / steps);
            s.rb = float(r0 + (r1 - r0) * double(i + 1) / steps);
            s.parent = parent;
            s.order = order;
            tree.segments.push_back(s);
            parent = int(tree.segments.size()) - 1;
            p = q;
            if (points) points->push_back(p);
        }
        return parent;
    }

    void leaves(const std::vector<dvec3>& pts, const dvec3& crownCenter, int count) {
        for (int i = 0; i < count; ++i) {
            const double t = rng.range(0.35, 1.0) * double(pts.size() - 1);
            const size_t k = std::min(size_t(t), pts.size() - 2);
            const dvec3 along = normalize(pts[k + 1] - pts[k]);
            const dvec3 at = pts[k] + (pts[k + 1] - pts[k]) * (t - double(k));
            const dvec3 side = rotateAround(perpendicular(along), along, rng.range(0, 2 * kPi));
            LeafCard c;
            c.size = float(sp.leafSize * rng.range(0.75, 1.25));
            c.center = f3(at + side * (c.size * 0.45));
            // Cards lean outward from the crown so the crown lights like a volume.
            const dvec3 out = normalize(at - crownCenter + dvec3{0, 0.3, 0});
            c.normal = f3(normalize(out * 0.7 + side * 0.3 + dvec3{0, 0.4, 0}));
            c.up = f3(normalize(along * 0.6 + side * 0.4 + dvec3{0, sp.gravity < 0 ? -0.3 : 0.1, 0}));
            tree.leaves.push_back(c);
        }
    }
};

}  // namespace

const char* treeKindName(TreeKind k) {
    switch (k) {
        case TreeKind::Veyl: return "Veyl";
        case TreeKind::Ombrel: return "Ombrel";
        case TreeKind::Sarn: return "Sarn";
        default: return "?";
    }
}

TreeSkeleton growTree(TreeKind kind, uint64_t seed) {
    const Species& sp = kSpecies[int(kind) % int(TreeKind::Count)];
    TreeSkeleton tree;
    tree.kind = kind;
    Grower g{sp, Rng{mix64(seed * 31 + uint64_t(kind) + 1)}, tree};

    const double H = g.rng.range(sp.height[0], sp.height[1]);
    const double r0 = H * sp.trunkRadius * g.rng.range(0.9, 1.15);
    const dvec3 lean = normalize(dvec3{g.rng.range(-0.06, 0.06), 1.0, g.rng.range(-0.06, 0.06)});
    std::vector<dvec3> trunk;
    g.limb({0, 0, 0}, lean, H, r0, r0 * 0.12, -1, 0, 0.3, &trunk);
    const int trunkSegments = int(tree.segments.size());
    tree.height = float(trunk.back().y);
    tree.trunkRadius = float(r0);
    const dvec3 crownCenter{0, H * (sp.crownStart + 1.0) * 0.5, 0};

    const int nb = int(std::lround(g.rng.range(sp.branches[0], sp.branches[1])));
    double azimuth = g.rng.range(0, 2 * kPi);
    double crown = 0;
    for (int b = 0; b < nb; ++b) {
        const double t = sp.crownStart + (0.97 - sp.crownStart) * (double(b) + g.rng.range(0.2, 0.8)) / nb;  // up the trunk
        const size_t k = std::min(size_t(t * double(trunk.size() - 1)), trunk.size() - 2);
        const dvec3 at = trunk[k] + (trunk[k + 1] - trunk[k]) * (t * double(trunk.size() - 1) - double(k));
        const dvec3 axis = normalize(trunk[k + 1] - trunk[k]);
        azimuth += 2.39996 + g.rng.range(-0.3, 0.3);  // golden angle around the trunk
        const double tilt = sp.branchAngle[1] + (sp.branchAngle[0] - sp.branchAngle[1]) * (t - sp.crownStart) / (1.0 - sp.crownStart);
        const dvec3 radial = rotateAround(perpendicular(axis), axis, azimuth);
        const dvec3 dir = normalize(axis * std::cos(tilt) + radial * std::sin(tilt));
        double len = sp.branchLength * H * g.rng.range(0.8, 1.15);
        len *= sp.cone ? (1.05 - t) * 1.4 : (1.0 - 0.45 * (t - sp.crownStart) / (1.0 - sp.crownStart));
        const double br = std::max(0.015, r0 * (sp.cone ? 0.25 : 0.45) * (1.05 - t));
        std::vector<dvec3> pts;
        const int parent = std::min(trunkSegments - 1, int(t * trunkSegments));
        g.limb(at, dir, len, br, br * 0.2, parent, 1, sp.gravity, &pts);
        const int branchStart = int(tree.segments.size()) - int(pts.size()) + 1;
        crown = std::max(crown, std::hypot(pts.back().x, pts.back().z));

        const int nt = int(std::lround(g.rng.range(sp.twigs[0], sp.twigs[1])));
        for (int w = 0; w < nt; ++w) {
            const double s = g.rng.range(0.25, 0.95);
            const size_t j = std::min(size_t(s * double(pts.size() - 1)), pts.size() - 2);
            const dvec3 a = pts[j] + (pts[j + 1] - pts[j]) * (s * double(pts.size() - 1) - double(j));
            const dvec3 ba = normalize(pts[j + 1] - pts[j]);
            const dvec3 tw = normalize(ba * 0.55 + rotateAround(perpendicular(ba), ba, g.rng.range(0, 2 * kPi)) * 0.8);
            std::vector<dvec3> twig;
            g.limb(a, tw, len * sp.twigLength * g.rng.range(0.7, 1.2), br * 0.35, std::min(0.006, br * 0.175), branchStart + int(j), 2,
                   sp.gravity, &twig);
            g.leaves(twig, crownCenter, int(std::lround(g.rng.range(sp.leavesPerTwig[0], sp.leavesPerTwig[1]))));
        }
        g.leaves(pts, crownCenter, sp.leavesPerTwig[0]);
    }
    tree.crownRadius = float(crown);
    return tree;
}

void treesInArea(const Terrain& terrain, double x0, double z0, double x1, double z1, std::vector<TreeInstance>& out) {
    const uint64_t seed = terrain.seed();
    const int64_t cx0 = int64_t(std::floor(x0 / kTreeCell)), cx1 = int64_t(std::ceil(x1 / kTreeCell));
    const int64_t cz0 = int64_t(std::floor(z0 / kTreeCell)), cz1 = int64_t(std::ceil(z1 / kTreeCell));
    const dvec3 fire = terrain.campfirePosition();
    std::vector<Pond> ponds;
    terrain.pondsNear((x0 + x1) * 0.5, (z0 + z1) * 0.5, std::hypot(x1 - x0, z1 - z0) * 0.5 + 60.0, ponds);
    for (int64_t cz = cz0; cz < cz1; ++cz)
        for (int64_t cx = cx0; cx < cx1; ++cx) {
            const uint64_t h = hashLattice(cx, 0x7ee, cz, seed);
            const double x = (double(cx) + 0.1 + 0.8 * hashToUnit(mix64(h + 1))) * kTreeCell;
            const double z = (double(cz) + 0.1 + 0.8 * hashToUnit(mix64(h + 2))) * kTreeCell;
            if (x < x0 || x >= x1 || z < z0 || z >= z1) continue;
            // Patches of forest, and a few lone trees in the open.
            const double forest = smoothstep(0.0, 0.4, fbm2(x, z, 380.0, 4, 0.5, mix64(seed ^ 0x30)));
            if (hashToUnit(h) > 0.025 + 0.7 * forest) continue;
            if (std::hypot(x, z) < 16.0 || std::hypot(x - fire.x, z - fire.z) < 9.0) continue;  // the clearing
            bool wet = false;
            for (const Pond& p : ponds) wet |= p.normalizedDistance(x, z) < 1.2;
            if (wet) continue;
            const TerrainSample s = terrain.sample(x, z);
            if (s.underwater() || s.materials.rock > 0.3f || s.normal.y < 0.82) continue;
            TreeInstance t;
            t.position = {x, s.height - 0.08, z};
            t.yaw = float(hashToUnit(mix64(h + 3)) * 2.0 * kPi);
            t.scale = float(0.75 + 0.5 * hashToUnit(mix64(h + 4)));
            // Species by region, mixed at the edges.
            const double region = fbm2(x, z, 900.0, 2, 0.5, mix64(seed ^ 0x31)) + (hashToUnit(mix64(h + 5)) - 0.5) * 0.5;
            t.kind = region < -0.12 ? TreeKind::Sarn : (region < 0.15 ? TreeKind::Veyl : TreeKind::Ombrel);
            t.variant = uint8_t(mix64(h + 6) % kTreeVariants);
            out.push_back(t);
        }
}

}  // namespace taelia
