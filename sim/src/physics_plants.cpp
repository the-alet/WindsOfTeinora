// Plants as elastic colliders: trunks and boughs are springy PhysX capsules; thin twigs and leaf sprays yield, each
// taking from the ball the share of momentum its own (effective) mass can take, so a twig bends away and slows the
// ball a little, a crown full of leaves slows it a lot, and a bough throws it back.
#include <algorithm>
#include <cmath>

#include "physics_internal.h"
#include "taelia/shared_constants.h"
#include "taelia/sim/hash.h"
#include "taelia/sim/terrain.h"

using namespace physx;

namespace taelia {

using namespace phys;

namespace {

constexpr double kThick = 0.03;          // m: thicker branches (at scale 1) are capsules, thinner ones yield
constexpr double kWoodDensity = 800.0;   // kg/m3, green wood
constexpr double kTwigShare = 0.3;       // of the twig beyond the hit that a ball feels (a cantilever bending)
constexpr double kTwigBounce = 0.25;     // restitution against a twig
constexpr double kSprayMass = 0.010;     // kg of leaves a ball sweeps aside crossing a card 0.9 m across
constexpr double kGridCell = 0.6;        // m, in the tree's own frame
constexpr int64_t kKeepTreeSteps = 600;

// The tree's frame: turned by yaw (from +z toward +x, as tree.hlsl draws it) and scaled.
dvec3 turnToTree(const TreeInstance& t, const dvec3& d) {
    const double c = std::cos(t.yaw), s = std::sin(t.yaw);
    return {d.x * c - d.z * s, d.y, d.x * s + d.z * c};
}

dvec3 turnToWorld(const TreeInstance& t, const dvec3& d) {
    const double c = std::cos(t.yaw), s = std::sin(t.yaw);
    return {d.x * c + d.z * s, d.y, -d.x * s + d.z * c};
}

dvec3 toTree(const TreeInstance& t, const dvec3& p) { return turnToTree(t, p - t.position) * (1.0 / t.scale); }

dvec3 dv3(const fvec3& v) { return {double(v.x), double(v.y), double(v.z)}; }

int64_t treeCell(double v) { return int64_t(std::floor(v / kTreeCell)); }

// Closest points of segments p0-p1 and q0-q1 (Ericson 5.1.9): squared distance; s and u along each, 0..1.
double closestSegments(const dvec3& p0, const dvec3& p1, const dvec3& q0, const dvec3& q1, double& s, double& u) {
    const dvec3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
    const double a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    if (a <= 1e-12 && e <= 1e-12) {
        s = u = 0.0;
        return dot(r, r);
    }
    if (a <= 1e-12) {
        s = 0.0;
        u = clampd(f / e, 0.0, 1.0);
    } else {
        const double c = dot(d1, r);
        if (e <= 1e-12) {
            u = 0.0;
            s = clampd(-c / a, 0.0, 1.0);
        } else {
            const double b = dot(d1, d2), den = a * e - b * b;
            s = den > 1e-12 ? clampd((b * f - c * e) / den, 0.0, 1.0) : 0.0;
            u = (b * s + f) / e;
            if (u < 0.0) {
                u = 0.0;
                s = clampd(-c / a, 0.0, 1.0);
            } else if (u > 1.0) {
                u = 1.0;
                s = clampd((b - c) / a, 0.0, 1.0);
            }
        }
    }
    const dvec3 d = (p0 + d1 * s) - (q0 + d2 * u);
    return dot(d, d);
}

double pointSegment(const dvec3& p, const dvec3& a, const dvec3& b) {
    const dvec3 ab = b - a;
    const double t = clampd(dot(p - a, ab) / std::max(dot(ab, ab), 1e-12), 0.0, 1.0);
    const dvec3 d = p - (a + ab * t);
    return dot(d, d);
}

}  // namespace

SkeletonGrid& Physics::Impl::grid(int sk) {
    SkeletonGrid& g = grids[size_t(sk)];
    if (g.skeleton) return g;
    const TreeKind kind = TreeKind(sk / kTreeVariants);
    const int variant = sk % kTreeVariants;
    skeletons[size_t(sk)] = growTree(kind, treeSkeletonSeed(kind, variant));
    const TreeSkeleton& tree = skeletons[size_t(sk)];
    g.skeleton = &tree;
    const auto thin = [&](int32_t i) {
        const TreeSegment& s = tree.segments[size_t(i)];
        return std::max(s.ra, s.rb) < kThick;
    };
    for (int32_t i = 0; i < int32_t(tree.segments.size()); ++i)
        if (thin(i)) g.thin.push_back(i);

    // Twigs: a thin segment continuing a thin parent of the same order belongs to the parent's twig. A ball hitting
    // a twig moves the part beyond the hit, out to the tip: sum that mass from the tips back (children come after
    // their parents).
    std::vector<int32_t> elementOf(tree.segments.size(), -1);
    g.chain.resize(g.thin.size());
    g.beyond.assign(g.thin.size(), 0.0);
    int32_t chains = 0;
    const auto continues = [&](const TreeSegment& s) {
        return s.parent >= 0 && thin(s.parent) && tree.segments[size_t(s.parent)].order == s.order;
    };
    for (size_t e = 0; e < g.thin.size(); ++e) {
        const TreeSegment& s = tree.segments[size_t(g.thin[e])];
        elementOf[size_t(g.thin[e])] = int32_t(e);
        const int32_t up = continues(s) ? elementOf[size_t(s.parent)] : -1;
        g.chain[e] = up >= 0 ? g.chain[size_t(up)] : chains++;
        const double r = 0.5 * double(s.ra + s.rb);
        g.beyond[e] = kWoodDensity * kPi * r * r * length(dv3(s.b) - dv3(s.a));
    }
    for (size_t e = g.thin.size(); e-- > 0;) {
        const TreeSegment& s = tree.segments[size_t(g.thin[e])];
        if (!continues(s)) continue;
        const int32_t up = elementOf[size_t(s.parent)];
        if (up >= 0 && size_t(up) < e) g.beyond[size_t(up)] += g.beyond[e];
    }

    // Each element's box (grown by its own radius), then a grid of them.
    const size_t count = g.thin.size() + tree.leaves.size();
    std::vector<dvec3> lo(count), hi(count);
    double blo[3] = {1e30, 1e30, 1e30}, bhi[3] = {-1e30, -1e30, -1e30};
    for (size_t e = 0; e < count; ++e) {
        if (e < g.thin.size()) {
            const TreeSegment& s = tree.segments[size_t(g.thin[e])];
            const double r = std::max(s.ra, s.rb);
            const dvec3 a = dv3(s.a), b = dv3(s.b);
            lo[e] = dvec3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)} - dvec3{r, r, r};
            hi[e] = dvec3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)} + dvec3{r, r, r};
        } else {
            const LeafCard& l = tree.leaves[e - g.thin.size()];
            const double r = 0.5 * l.size;
            lo[e] = dv3(l.center) - dvec3{r, r, r};
            hi[e] = dv3(l.center) + dvec3{r, r, r};
        }
        const double el[3] = {lo[e].x, lo[e].y, lo[e].z}, eh[3] = {hi[e].x, hi[e].y, hi[e].z};
        for (int k = 0; k < 3; ++k) {
            blo[k] = std::min(blo[k], el[k]);
            bhi[k] = std::max(bhi[k], eh[k]);
        }
    }
    for (const TreeSegment& s : tree.segments) {
        const double r = std::max(s.ra, s.rb);
        g.reach = std::max({g.reach, std::hypot(s.a.x, s.a.z) + r, std::hypot(s.b.x, s.b.z) + r});
    }
    for (const LeafCard& l : tree.leaves) g.reach = std::max(g.reach, std::hypot(l.center.x, l.center.z) + 0.5 * l.size);
    if (count == 0) return g;
    for (int k = 0; k < 3; ++k) {
        g.lo[k] = blo[k];
        g.dim[k] = std::clamp(int(std::ceil((bhi[k] - blo[k]) / kGridCell)), 1, 96);
    }
    const auto cellRange = [&](const dvec3& a, const dvec3& b, int c0[3], int c1[3]) {
        const double av[3] = {a.x, a.y, a.z}, bv[3] = {b.x, b.y, b.z};
        for (int k = 0; k < 3; ++k) {
            c0[k] = std::clamp(int(std::floor((av[k] - g.lo[k]) / kGridCell)), 0, g.dim[k] - 1);
            c1[k] = std::clamp(int(std::floor((bv[k] - g.lo[k]) / kGridCell)), 0, g.dim[k] - 1);
        }
    };
    const size_t cells = size_t(g.dim[0]) * g.dim[1] * g.dim[2];
    g.cellStart.assign(cells + 1, 0);
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<uint32_t> fill;
        if (pass == 1) {
            for (size_t c = 0; c < cells; ++c) g.cellStart[c + 1] += g.cellStart[c];
            g.items.assign(g.cellStart[cells], 0);
            fill.assign(g.cellStart.begin(), g.cellStart.end() - 1);
        }
        for (size_t e = 0; e < count; ++e) {
            int c0[3], c1[3];
            cellRange(lo[e], hi[e], c0, c1);
            for (int x = c0[0]; x <= c1[0]; ++x)
                for (int y = c0[1]; y <= c1[1]; ++y)
                    for (int z = c0[2]; z <= c1[2]; ++z) {
                        const size_t c = (size_t(x) * g.dim[1] + y) * g.dim[2] + z;
                        if (pass == 0) ++g.cellStart[c + 1];
                        else g.items[fill[c]++] = uint32_t(e);
                    }
        }
    }
    g.stamp.assign(count, 0);
    return g;
}

// Trunks and boughs of one tree as capsules on a static actor.
void Physics::Impl::addTree(const TreeInstance& t, const CellKey& key) {
    TreeBody& tb = trees[key];
    tb.tree = t;
    tb.key = hashCombine(uint64_t(key.first), uint64_t(key.second));
    tb.skeleton = int(t.kind) * kTreeVariants + int(t.variant);
    tb.lastUsed = stepIndex;
    const SkeletonGrid& g = grid(tb.skeleton);
    const TreeSkeleton& sk = *g.skeleton;
    treeReach = std::max(treeReach, g.reach * t.scale + k::BALL_RADIUS);
    tb.actor = px->createRigidStatic(PxTransform(toPx(t.position - origin)));
    for (const TreeSegment& seg : sk.segments) {
        if (std::max(seg.ra, seg.rb) < kThick) continue;
        const dvec3 a = turnToWorld(t, dv3(seg.a)) * double(t.scale);
        const dvec3 b = turnToWorld(t, dv3(seg.b)) * double(t.scale);
        const double len = length(b - a), r = 0.5 * double(seg.ra + seg.rb) * t.scale;
        PxShape* shape = nullptr;
        if (len < 1e-3) {
            shape = PxRigidActorExt::createExclusiveShape(*tb.actor, PxSphereGeometry(float(r)), *barkMaterial);
            shape->setLocalPose(PxTransform(toPx(a)));
        } else {
            shape = PxRigidActorExt::createExclusiveShape(*tb.actor, PxCapsuleGeometry(float(r), float(0.5 * len)), *barkMaterial);
            const PxQuat q = PxShortestRotation(PxVec3(1.0f, 0.0f, 0.0f), toPx((b - a) * (1.0 / len)));
            shape->setLocalPose(PxTransform(toPx((a + b) * 0.5), q));
        }
        shape->setContactOffset(0.02f);
        shape->setRestOffset(0.0f);
        ++tb.shapes;
    }
    scene->addActor(*tb.actor);
}

// Tree cells around p: known ones are only marked as wanted; unknown ones are worked out once, as one block.
void Physics::Impl::streamTrees(const dvec3& p, double radius) {
    if (!terrain) return;
    const int64_t x0 = treeCell(p.x - radius), x1 = treeCell(p.x + radius);
    const int64_t z0 = treeCell(p.z - radius), z1 = treeCell(p.z + radius);
    bool unknown = false;
    for (int64_t x = x0; x <= x1; ++x) {
        for (int64_t z = z0; z <= z1; ++z) {
            const CellKey key{x, z};
            if (auto it = trees.find(key); it != trees.end()) it->second.lastUsed = stepIndex;
            else if (auto e = emptyTreeCells.find(key); e != emptyTreeCells.end()) e->second = stepIndex;
            else unknown = true;
        }
    }
    if (!unknown) return;
    scratchTrees.clear();
    treesInArea(*terrain, double(x0) * kTreeCell, double(z0) * kTreeCell, double(x1 + 1) * kTreeCell,
                double(z1 + 1) * kTreeCell, scratchTrees);
    for (const TreeInstance& t : scratchTrees) {
        const CellKey key{treeCell(t.position.x), treeCell(t.position.z)};
        if (!trees.count(key)) addTree(t, key);
    }
    for (int64_t x = x0; x <= x1; ++x)
        for (int64_t z = z0; z <= z1; ++z)
            if (!trees.count(CellKey{x, z})) emptyTreeCells[CellKey{x, z}] = stepIndex;
}

void Physics::Impl::releaseTree(TreeBody& t) {
    if (t.actor) t.actor->release();
    t.actor = nullptr;
}

void Physics::Impl::releaseUnusedTrees() {
    for (auto it = trees.begin(); it != trees.end();) {
        if (stepIndex - it->second.lastUsed > kKeepTreeSteps) {
            releaseTree(it->second);
            it = trees.erase(it);
        } else {
            ++it;
        }
    }
    std::erase_if(emptyTreeCells, [&](const auto& e) { return stepIndex - e.second > kKeepTreeSteps; });
}

// The ball moved from -> to in the last PhysX step: every thin twig and leaf spray it passed gives way, once.
void Physics::Impl::plantContacts(BallBody& b, const dvec3& from, const dvec3& to) {
    b.touchingNext.clear();
    if (trees.empty()) {
        b.touching.clear();
        return;
    }
    const double R = k::BALL_RADIUS, M = k::BALL_MASS;
    dvec3 v = b.state.velocity;
    bool changed = false;
    plantHits.clear();
    const int64_t cx0 = treeCell(std::min(from.x, to.x) - treeReach), cx1 = treeCell(std::max(from.x, to.x) + treeReach);
    const int64_t cz0 = treeCell(std::min(from.z, to.z) - treeReach), cz1 = treeCell(std::max(from.z, to.z) + treeReach);
    for (int64_t cx = cx0; cx <= cx1; ++cx) {
        for (int64_t cz = cz0; cz <= cz1; ++cz) {
            const auto found = trees.find(CellKey{cx, cz});
            if (found == trees.end()) continue;
            TreeBody& tb = found->second;
            const TreeInstance& t = tb.tree;
            SkeletonGrid& g = grid(tb.skeleton);
            if (g.cellStart.empty()) continue;
            const TreeSkeleton& sk = *g.skeleton;

            const dvec3 l0 = toTree(t, from), l1 = toTree(t, to);
            const double rl = R / t.scale;
            const double lov[3] = {std::min(l0.x, l1.x) - rl, std::min(l0.y, l1.y) - rl, std::min(l0.z, l1.z) - rl};
            const double hiv[3] = {std::max(l0.x, l1.x) + rl, std::max(l0.y, l1.y) + rl, std::max(l0.z, l1.z) + rl};
            int c0[3], c1[3];
            bool outside = false;
            for (int k = 0; k < 3; ++k) {
                const double a = (lov[k] - g.lo[k]) / kGridCell, z = (hiv[k] - g.lo[k]) / kGridCell;
                if (z < 0.0 || a >= double(g.dim[k])) outside = true;
                c0[k] = std::clamp(int(std::floor(a)), 0, g.dim[k] - 1);
                c1[k] = std::clamp(int(std::floor(z)), 0, g.dim[k] - 1);
            }
            if (outside) continue;
            if (++g.query == 0) {
                std::fill(g.stamp.begin(), g.stamp.end(), 0u);
                g.query = 1;
            }
            for (int x = c0[0]; x <= c1[0]; ++x)
                for (int y = c0[1]; y <= c1[1]; ++y)
                    for (int z = c0[2]; z <= c1[2]; ++z) {
                        const size_t c = (size_t(x) * g.dim[1] + y) * g.dim[2] + z;
                        for (uint32_t i = g.cellStart[c]; i < g.cellStart[c + 1]; ++i) {
                            const uint32_t e = g.items[i];
                            if (g.stamp[e] == g.query) continue;
                            g.stamp[e] = g.query;
                            const bool twig = e < g.thin.size();
                            const dvec3 vl = turnToTree(t, v);  // the ball's velocity, tree frame
                            dvec3 nl;          // the push on the ball, tree frame
                            double share = 0;  // of the momentum toward the plant that it takes
                            double bounce = 0, rub = 0;
                            uint64_t id = 0;
                            double along = 0;
                            if (twig) {
                                const TreeSegment& s = sk.segments[size_t(g.thin[e])];
                                const dvec3 a = dv3(s.a), bb = dv3(s.b);
                                double sp, up;
                                const double d2 = closestSegments(l0, l1, a, bb, sp, up);
                                const double r = double(s.ra) + (double(s.rb) - double(s.ra)) * up;
                                const double reach = rl + r;
                                if (d2 >= reach * reach) continue;
                                // The normal where the ball first touched the twig: across the twig, from the line
                                // the ball came along (a graze pushes it aside, a head-on hit pushes it back).
                                const dvec3 axis = normalize(bb - a);
                                const dvec3 across = vl - axis * dot(vl, axis);
                                const double sa = length(across);
                                if (sa < 1e-6) continue;  // moving along the twig
                                const dvec3 u = across * (1.0 / sa);
                                const dvec3 rel = l0 - a;
                                const dvec3 relAcross = rel - axis * dot(rel, axis);
                                const dvec3 miss = relAcross - u * dot(relAcross, u);
                                const double d = length(miss), q = std::min(d / reach, 1.0);
                                nl = d > 1e-9 ? miss * (q / d) - u * std::sqrt(1.0 - q * q) : u * -1.0;
                                const double mass = kTwigShare * g.beyond[e] * double(t.scale) * t.scale * t.scale;
                                share = mass / (M + mass);
                                bounce = kTwigBounce;
                                rub = 0.25;
                                id = hashCombine(tb.key, 0x40000000u + uint64_t(g.chain[e]));
                                along = sp;
                            } else {
                                const LeafCard& l = sk.leaves[e - g.thin.size()];
                                const double reachLeaf = rl + 0.35 * double(l.size);
                                if (pointSegment(dv3(l.center), l0, l1) >= reachLeaf * reachLeaf) continue;
                                nl = dv3(l.normal);
                                if (dot(nl, vl) > 0.0) nl = nl * -1.0;
                                const double mass = kSprayMass * double(l.size) * t.scale / 0.9;  // the strip it crosses
                                share = mass / (M + mass);
                                rub = 0.4;
                                id = hashCombine(tb.key, uint64_t(e));
                                const dvec3 d = l1 - l0;
                                along = clampd(dot(dv3(l.center) - l0, d) / std::max(dot(d, d), 1e-12), 0.0, 1.0);
                            }
                            plantHits.push_back({along, id, nl, share, bounce, rub, twig, &tb.tree});
                        }
                    }
        }
    }
    // In order along the path; a twig pushes once, from where the ball first reached it.
    std::stable_sort(plantHits.begin(), plantHits.end(), [](const PlantHit& x, const PlantHit& y) { return x.along < y.along; });
    for (const PlantHit& hit : plantHits) {
        const bool seen = std::find(b.touchingNext.begin(), b.touchingNext.end(), hit.id) != b.touchingNext.end();
        if (!seen) b.touchingNext.push_back(hit.id);
        if (seen || std::find(b.touching.begin(), b.touching.end(), hit.id) != b.touching.end()) continue;
        const dvec3 n = normalize(turnToWorld(*hit.tree, hit.normal));
        const double vn = dot(v, n);
        if (vn >= 0.0) continue;
        v = v - n * ((1.0 + hit.bounce) * hit.share * vn);
        const dvec3 vt = v - n * dot(v, n);
        v = v - vt * (hit.rub * hit.share);
        changed = true;
        if (hit.twig) ++stats.twigHits;
        else ++stats.leafHits;
    }
    b.touching.swap(b.touchingNext);
    if (changed) {
        b.state.velocity = v;
        b.actor->setLinearVelocity(toPx(v));
    }
}

}  // namespace taelia
