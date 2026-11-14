#include "taelia/sim/physics.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "physics_internal.h"
#include "taelia/shared_constants.h"
#include "taelia/sim/hash.h"
#include "taelia/sim/magic_kinetics.h"
#include "taelia/sim/terrain.h"
#include "taelia/sim/wind.h"

using namespace physx;

namespace taelia {

using namespace phys;

namespace {

constexpr double kGravity = 9.81;
constexpr double kAirDensity = 1.2;       // kg/m3
constexpr double kWaterDensity = 1000.0;  // kg/m3
constexpr double kDrag = 0.47;            // drag coefficient of a smooth ball
constexpr double kGrassDrag = 0.35;       // 1/m: how hard grass blades hold a ball pushing through them
constexpr double kTileSize = 32.0;        // m, one height field tile
constexpr int kTileSamples = 33;          // per side, 1 m apart; edge rows are shared with the neighbours
constexpr int64_t kKeepSteps = 600;       // ground and trees no ball came near for 10 s go
constexpr double kOriginReach = 4096.0;   // a throw farther than this from the PhysX origin moves the origin
constexpr double kTreeStream = 13.0;      // m: the widest crown, twigs and leaves included, at the largest scale

// Soft ground: restitution well below a hard surface's (a compliant spring-damper contact would be softer still,
// but PhysX solves it implicitly and at our step it swallowed the bounce whole), friction, rolling resistance.
struct SurfaceSpec {
    double restitution, friction, rolling;
};
constexpr SurfaceSpec kSurfaces[kSurfaceCount] = {
    {0.45, 0.75, 0.10},   // grass
    {0.35, 0.80, 0.05},   // dirt
    {0.62, 0.60, 0.015},  // rock: hard
    {0.15, 0.90, 0.30},   // sand swallows the bounce
    {0.08, 0.90, 0.40},   // pond bed
};
constexpr SurfaceSpec kBark = {0.5, 0.60, 0.0};  // a bough springs the ball back
constexpr SurfaceSpec kBody = {0.3, 0.70, 0.0};  // cloth over flesh takes much of the bounce

class ErrorsToStderr : public PxErrorCallback {
public:
    void reportError(PxErrorCode::Enum code, const char* message, const char* file, int line) override {
        std::fprintf(stderr, "PhysX error %d: %s (%s:%d)\n", int(code), message, file, line);
    }
};

// PhysX allows one foundation per process: every world shares it and one PxPhysics.
struct Runtime {
    std::mutex mutex;
    PxDefaultAllocator allocator;
    ErrorsToStderr errors;
    PxFoundation* foundation = nullptr;
    PxPhysics* physics = nullptr;
    int users = 0;
};

Runtime& runtime() {
    static Runtime r;
    return r;
}

PxPhysics* acquirePhysics() {
    Runtime& r = runtime();
    std::lock_guard lock(r.mutex);
    if (r.users++ == 0) {
        r.foundation = PxCreateFoundation(PX_PHYSICS_VERSION, r.allocator, r.errors);
        r.physics = PxCreatePhysics(PX_PHYSICS_VERSION, *r.foundation, PxTolerancesScale(), false, nullptr);
    }
    return r.physics;
}

void releasePhysics() {
    Runtime& r = runtime();
    std::lock_guard lock(r.mutex);
    if (--r.users == 0) {
        r.physics->release();
        r.foundation->release();
        r.physics = nullptr;
        r.foundation = nullptr;
    }
}

PxFilterFlags contactFilter(PxFilterObjectAttributes, PxFilterData, PxFilterObjectAttributes, PxFilterData,
                            PxPairFlags& flags, const void*, PxU32) {
    flags = PxPairFlag::eCONTACT_DEFAULT | PxPairFlag::eDETECT_CCD_CONTACT;
    return PxFilterFlag::eDEFAULT;
}

PxMaterial* makeMaterial(PxPhysics* px, const SurfaceSpec& s) {
    PxMaterial* m = px->createMaterial(float(s.friction), float(s.friction * 0.85), float(s.restitution));
    m->setRestitutionCombineMode(PxCombineMode::eMIN);  // the ground decides how much comes back
    return m;
}

int64_t cellOf(double v, double size) { return int64_t(std::floor(v / size)); }

dvec3 originNear(const dvec3& p) { return {std::round(p.x / 1024.0) * 1024.0, 0.0, std::round(p.z / 1024.0) * 1024.0}; }

uint64_t bits(double v) { return std::bit_cast<uint64_t>(v); }

}  // namespace

// ---------------------------------------------------------------------------------------------- the scene

Physics::Impl::Impl(const Terrain* t, const WindField* w, uint64_t s) : terrain(t), wind(w), seed(s) {
    px = acquirePhysics();
    dispatcher = PxDefaultCpuDispatcherCreate(0);  // runs on the calling thread: deterministic
    PxSceneDesc sd(px->getTolerancesScale());
    sd.gravity = PxVec3(0.0f, float(-kGravity), 0.0f);
    sd.cpuDispatcher = dispatcher;
    sd.filterShader = contactFilter;
    sd.flags |= PxSceneFlag::eENABLE_ENHANCED_DETERMINISM;
    sd.flags |= PxSceneFlag::eENABLE_CCD;
    sd.bounceThresholdVelocity = 0.25f;
    scene = px->createScene(sd);
    ballMaterial = px->createMaterial(0.7f, 0.6f, 0.85f);
    barkMaterial = makeMaterial(px, kBark);
    bodyMaterial = makeMaterial(px, kBody);
    for (int i = 0; i < kSurfaceCount; ++i) surfaces[i] = makeMaterial(px, kSurfaces[i]);
    if (!terrain) {
        flatGround = PxCreatePlane(*px, PxPlane(0.0f, 1.0f, 0.0f, 0.0f), *surfaces[kGrass]);
        scene->addActor(*flatGround);
    }
    const size_t kinds = size_t(TreeKind::Count) * kTreeVariants;
    skeletons.resize(kinds);
    grids.resize(kinds);
}

Physics::Impl::~Impl() {
    for (BallBody& b : balls) b.actor->release();
    for (auto& [key, t] : trees) releaseTree(t);
    for (auto& [key, tile] : tiles) {
        if (tile.actor) tile.actor->release();
        if (tile.field) tile.field->release();
    }
    for (auto& [id, r] : rocks) r->release();
    for (auto& [id, c] : capsules) c->release();
    if (flatGround) flatGround->release();
    scene->release();
    dispatcher->release();
    ballMaterial->release();
    barkMaterial->release();
    bodyMaterial->release();
    for (PxMaterial* m : surfaces) m->release();
    releasePhysics();
}

namespace {

constexpr int kCells = (kTileSamples - 1) / 2;  // 2 m cells per tile side
constexpr int kTileBudget = 1;                  // tiles built per step besides the one under a ball
constexpr int kSettleSubsteps = 12;             // slow on the ground this long before a ball settles
constexpr int64_t kWakeCheckSteps = 15;         // how often a resting ball asks whether the wind can move it

Surface surfaceOf(const GroundMaterials& m, bool underwater) {
    if (underwater) return kMud;
    Surface s = kGrass;
    float best = m.grass;
    if (m.dirt > best) best = m.dirt, s = kDirt;
    if (m.rock > best) best = m.rock, s = kRock;
    if (m.sand > best) best = m.sand, s = kSand;
    return s;
}

double grassOf(const GroundMaterials& m, bool underwater) {
    return underwater ? 0.0 : (0.04 + 0.45 * double(m.meadow)) * double(m.grass);
}

// What the ground holds a ball against (as a slope's tangent): rolling resistance, and tall grass cradling it.
double holdOf(const Ground& g) { return kSurfaces[g.surface].rolling + 1.2 * std::max(g.grassHeight - 0.04, 0.0); }

// The wind's push on a ball, in units of its weight.
double windPush(const dvec3& air) {
    const double r = k::BALL_RADIUS;
    return 0.5 * kAirDensity * kDrag * kPi * r * r * dot(air, air) / (k::BALL_MASS * kGravity);
}

}  // namespace

// The ground under a ball: height and normal of the very triangles PhysX collides with, the rest from the tile.
Ground Physics::Impl::groundUnder(const dvec3& p) const {
    Ground g;
    if (!terrain) return g;
    const auto it = tiles.find(CellKey{cellOf(p.x, kTileSize), cellOf(p.z, kTileSize)});
    if (it == tiles.end() || !it->second.actor) {
        const TerrainSample s = terrain->sample(p.x, p.z);  // not built yet: ask the terrain itself
        g.height = s.height;
        g.normal = s.normal;
        g.water = s.waterLevel;
        g.surface = surfaceOf(s.materials, s.underwater());
        g.grassHeight = grassOf(s.materials, s.underwater());
        return g;
    }
    const Tile& t = it->second;
    // Inside the tile: a ray exactly on its edge may miss once rounded to float.
    const double x = clampd(p.x, t.x0 + 1e-3, t.x0 + kTileSize - 1e-3), z = clampd(p.z, t.z0 + 1e-3, t.z0 + kTileSize - 1e-3);
    const PxVec3 from(float(x - origin.x), float(t.hi + 1.0 - origin.y), float(z - origin.z));
    PxGeomRaycastHit hit;
    if (PxGeometryQuery::raycast(from, PxVec3(0.0f, -1.0f, 0.0f), t.shape->getGeometry(), t.actor->getGlobalPose(),
                                 float(t.hi - t.lo + 2.0), PxHitFlag::ePOSITION | PxHitFlag::eNORMAL, 1, &hit)) {
        g.height = double(hit.position.y) + origin.y;
        g.normal = toD(hit.normal);
    } else {
        const TerrainSample s = terrain->sample(p.x, p.z);
        g.height = s.height;
        g.normal = s.normal;
    }
    const int i = std::clamp(int((p.x - t.x0) / 2.0), 0, kCells - 1);
    const int j = std::clamp(int((p.z - t.z0) / 2.0), 0, kCells - 1);
    const size_t c = size_t(i) * kCells + size_t(j);
    g.surface = Surface(t.surface[c]);
    g.grassHeight = t.grass[c];
    g.water = t.water[c];
    return g;
}

// Tiles a ball at p will need: the one under it is built at once, the others nearest first, a few per step.
void Physics::Impl::streamGround(const dvec3& p, double radius) {
    if (!terrain) return;
    const CellKey under{cellOf(p.x, kTileSize), cellOf(p.z, kTileSize)};
    std::vector<std::pair<double, CellKey>> wanted;
    for (int64_t ix = cellOf(p.x - radius, kTileSize); ix <= cellOf(p.x + radius, kTileSize); ++ix) {
        for (int64_t iz = cellOf(p.z - radius, kTileSize); iz <= cellOf(p.z + radius, kTileSize); ++iz) {
            const double dx = (double(ix) + 0.5) * kTileSize - p.x, dz = (double(iz) + 0.5) * kTileSize - p.z;
            wanted.push_back({dx * dx + dz * dz, CellKey{ix, iz}});
        }
    }
    std::sort(wanted.begin(), wanted.end());
    for (const auto& [d, key] : wanted) {
        Tile& tile = tiles[key];
        tile.lastUsed = stepIndex;
        if (tile.actor) continue;
        if (key != under && tilesBuiltThisStep >= kTileBudget) continue;
        buildTile(key, tile);
        if (key != under) ++tilesBuiltThisStep;
    }
}

// Heights every metre; per 2 m cell the terrain's own materials and pond level at its centre.
void Physics::Impl::buildTile(const CellKey& key, Tile& tile) {
    constexpr int n = kTileSamples;
    const double x0 = double(key.first) * kTileSize, z0 = double(key.second) * kTileSize;
    const double spacing = kTileSize / double(n - 1);
    std::vector<double> h(size_t(n) * n);
    double lo = 1e30, hi = -1e30;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            const double v = terrain->height(x0 + i * spacing, z0 + j * spacing);
            h[size_t(i) * n + j] = v;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    const auto at = [&](int i, int j) { return h[size_t(i) * n + size_t(j)]; };
    tile.surface.assign(size_t(kCells) * kCells, uint8_t(kGrass));
    tile.grass.assign(size_t(kCells) * kCells, 0.0f);
    tile.water.assign(size_t(kCells) * kCells, -1e30f);
    for (int i = 0; i < kCells; ++i) {
        for (int j = 0; j < kCells; ++j) {
            const int si = 2 * i + 1, sj = 2 * j + 1;
            const double hc = at(si, sj), x = x0 + si * spacing, z = z0 + sj * spacing;
            const dvec3 normal = normalize(dvec3{at(si - 1, sj) - at(si + 1, sj), 2.0 * spacing, at(si, sj - 1) - at(si, sj + 1)});
            // The pond reaches into the cell if any of its samples lies under it (the ball's own height decides
            // the rest); the centre being under it makes the cell pond bed.
            double lowest = hc;
            for (int di = -1; di <= 1; ++di)
                for (int dj = -1; dj <= 1; ++dj) lowest = std::min(lowest, at(si + di, sj + dj));
            const double pond = terrain->waterLevelAt(x, z, lowest);
            const bool wet = pond > hc;
            const GroundMaterials m = terrain->materials(x, z, hc, normal);
            const size_t c = size_t(i) * kCells + size_t(j);
            tile.surface[c] = uint8_t(surfaceOf(m, wet));
            tile.grass[c] = float(grassOf(m, wet));
            tile.water[c] = pond > lowest ? float(pond) : -1e30f;
        }
    }
    const double scale = std::max((hi - lo) / 30000.0, 0.002);
    std::vector<PxHeightFieldSample> samples(size_t(n) * n);
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            PxHeightFieldSample& s = samples[size_t(i) * n + j];
            s.height = PxI16(std::lround((at(i, j) - lo) / scale));
            const PxU8 m = tile.surface[size_t(std::min(i / 2, kCells - 1)) * kCells + size_t(std::min(j / 2, kCells - 1))];
            s.materialIndex0 = m;
            s.materialIndex1 = m;
        }
    }
    PxHeightFieldDesc desc;
    desc.format = PxHeightFieldFormat::eS16_TM;
    desc.nbRows = PxU32(n);     // rows run along x
    desc.nbColumns = PxU32(n);  // columns along z
    desc.samples.data = samples.data();
    desc.samples.stride = sizeof(PxHeightFieldSample);
    tile.field = PxCreateHeightField(desc, px->getPhysicsInsertionCallback());
    const PxHeightFieldGeometry geom(tile.field, PxMeshGeometryFlags(), float(scale), float(spacing), float(spacing));
    tile.actor = px->createRigidStatic(PxTransform(toPx(dvec3{x0, lo, z0} - origin)));
    tile.shape = PxRigidActorExt::createExclusiveShape(*tile.actor, geom, surfaces, PxU16(kSurfaceCount));
    tile.shape->setContactOffset(0.03f);
    tile.shape->setRestOffset(0.0f);
    scene->addActor(*tile.actor);
    tile.x0 = x0;
    tile.z0 = z0;
    tile.lo = lo;
    tile.hi = hi;
}

void Physics::Impl::releaseUnusedGround() {
    for (auto it = tiles.begin(); it != tiles.end();) {
        if (stepIndex - it->second.lastUsed > kKeepSteps) {
            if (it->second.actor) it->second.actor->release();
            if (it->second.field) it->second.field->release();
            it = tiles.erase(it);
        } else {
            ++it;
        }
    }
}

void Physics::Impl::removeBall(size_t i) {
    balls[i].actor->release();
    balls.erase(balls.begin() + std::ptrdiff_t(i));
}

// Our own forces, as velocity changes before PhysX's step (it adds gravity and contacts): drag and spin lift against
// the wind, buoyancy and drag in a pond, grass the ball pushes through, rolling resistance on the ground.
// Resistances are applied implicitly so no step, however large, overshoots.
void Physics::Impl::applyAirWaterGround(BallBody& b, double h) {
    const double R = k::BALL_RADIUS, M = k::BALL_MASS, area = kPi * R * R;
    const dvec3 p = b.state.position, v0 = b.state.velocity;
    dvec3 v = v0, w = b.state.spin;
    const Ground g = groundUnder(p);
    const double clearance = (p.y - g.height) * g.normal.y - R;  // gap under the ball, along the ground's normal
    const double depth = g.water > g.height ? clampd(g.water - (p.y - R), 0.0, 2.0 * R) : 0.0;
    const double wet = depth / (2.0 * R);
    b.state.inWater = depth > 0.0;

    // Air: drag against the wind, and spin lift on the part above water.
    const double base = b.state.inWater ? g.water : g.height;
    dvec3 air = wind ? wind->at(p, std::max(p.y - base, 0.0)) : dvec3{};
    double rhoMagic = 0, airShare = 0;
    dvec3 uMagic{};
    if (magic) {
        magic(p, rhoMagic, uMagic, airShare);
        air += (uMagic - air) * (double(k::MAGIC_WIND_SHARE) * airShare);  // moving air magic makes the wind
    }
    const dvec3 rel = v - air;
    const double speed = length(rel);
    if (speed > 1e-6) {
        // Lift = 0.5 rho A R (w x v): its coefficient R |w across v| / |v| saturates at 0.35 for fast spin.
        const dvec3 lift = cross(w, rel);
        const double across = length(lift) / speed;
        const double cap = across > 1e-9 ? std::min(1.0, 0.35 * speed / (R * across)) : 0.0;
        v += lift * (0.5 * kAirDensity * area * R / M * (1.0 - wet) * cap * h);
        const double cd = 0.5 * kAirDensity * kDrag * area / M * (1.0 - wet);
        v = (v - air) * (1.0 / (1.0 + cd * speed * h)) + air;
    }
    w = w * std::exp(-0.05 * h);

    // Water: Archimedes and drag on the wet part.
    if (depth > 0.0) {
        const double submerged = kPi * depth * depth * (3.0 * R - depth) / 3.0;
        v.y += kWaterDensity * submerged * kGravity / M * h;
        const double cw = 0.5 * kWaterDensity * kDrag * area * wet / M;
        v = v * (1.0 / (1.0 + cw * length(v) * h));
        w = w * std::exp(-3.0 * h);
    }

    // Grass: blades hold whatever part of the ball is down among them.
    if (depth == 0.0 && g.grassHeight > 0.0 && clearance < g.grassHeight) {
        const double share = clampd((g.grassHeight - std::max(clearance, 0.0)) / (2.0 * R), 0.0, 1.0);
        v = v * (1.0 / (1.0 + kGrassDrag * share * length(v) * h));
    }

    // Dense magic: past the effect threshold it drags the ball along with it (below it, nothing).
    if (magic) {
        const double over = magic::effectFactor(rhoMagic) > 0.0 ? rhoMagic - double(k::MAGIC_EFFECT_THRESHOLD) : 0.0;
        if (over > 0.0) {
            const dvec3 relm = v - uMagic;
            const double cm = 0.5 * kDrag * area * over * double(k::MAGIC_PUSH) / M;
            v = uMagic + relm * (1.0 / (1.0 + cm * length(relm) * h));
        }
    }

    // Rolling resistance: soft ground gives under a rolling ball; spin follows the slower roll.
    const bool onGround = depth == 0.0 && clearance < 0.012;
    if (onGround) {
        const dvec3 vt = v - g.normal * dot(v, g.normal);
        const double s = length(vt);
        const double lose = holdOf(g) * kGravity * h;  // tall grass holds a ball as well as slowing it
        if (s > 1e-9) {
            v = v - vt * (std::min(lose, s) / s);
            w = w * (std::max(s - lose, 0.0) / s);
        }
    }

    const dvec3 dv = v - v0, dw = w - b.state.spin;
    if (dot(dv, dv) > 1e-12 || dot(dw, dw) > 1e-10) {
        b.actor->setLinearVelocity(toPx(v), false);  // no autowake: PhysX may still let it fall asleep
        b.actor->setAngularVelocity(toPx(w), false);
    }

    // Settling: slow on the ground for a while, where the ground holds it against both its slope and the wind.
    const bool slow = onGround && dot(v, v) < 0.05 * 0.05 && dot(w, w) < 0.8 * 0.8;
    b.slowSubsteps = slow ? b.slowSubsteps + 1 : 0;
    const double slope = std::sqrt(std::max(1.0 - g.normal.y * g.normal.y, 0.0)) / std::max(g.normal.y, 0.05);
    const double hold = holdOf(g);
    if (b.slowSubsteps >= kSettleSubsteps && slope < 0.9 * hold && windPush(air) < 0.8 * hold) {
        b.actor->putToSleep();
        b.slowSubsteps = 0;
        b.rest = g;
        b.restKnown = true;
    }
}

void Physics::Impl::substep(double h, bool first) {
    const size_t n = balls.size();
    std::vector<dvec3> before(n);
    for (size_t i = 0; i < n; ++i) {
        BallBody& b = balls[i];
        before[i] = b.state.position;
        if (b.actor->isSleeping()) {
            if (!b.restKnown) {
                b.rest = groundUnder(b.state.position);
                b.restKnown = true;
            }
            // Now and then a resting ball asks whether the wind can move it (a gust stronger than the ground's hold).
            if (!wind || !first || stepIndex % kWakeCheckSteps != 0) continue;
            const bool floating = b.rest.water > b.rest.height;
            const double above = b.state.position.y - std::max(b.rest.height, b.rest.water);
            const double hold = floating ? 0.01 : holdOf(b.rest);  // water hardly holds a floating ball
            if (windPush(wind->at(b.state.position, std::max(above, 0.0))) < hold) continue;
            b.actor->wakeUp();
            b.slowSubsteps = 0;
        }
        b.restKnown = false;
        applyAirWaterGround(b, h);
    }
    std::vector<dvec3> vIn(n);
    for (size_t i = 0; i < n; ++i) vIn[i] = toD(balls[i].actor->getLinearVelocity());
    scene->simulate(float(h));
    scene->fetchResults(true);
    for (size_t i = 0; i < n; ++i) {
        BallBody& b = balls[i];
        b.state.squash *= std::exp(-h / double(k::BALL_SQUASH_TAU));
        const PxTransform pose = b.actor->getGlobalPose();
        b.state.position = toD(pose.p) + origin;
        b.state.rotation[0] = pose.q.x;
        b.state.rotation[1] = pose.q.y;
        b.state.rotation[2] = pose.q.z;
        b.state.rotation[3] = pose.q.w;
        b.state.velocity = toD(b.actor->getLinearVelocity());
        b.state.spin = toD(b.actor->getAngularVelocity());
        b.state.resting = b.actor->isSleeping();
        if (b.state.resting) continue;
        // A hit on the ground: what the ball lost into it along the ground's normal dents it (World); it squashes.
        {
            const Ground g = groundUnder(b.state.position);
            const double gap = (b.state.position.y - g.height) * g.normal.y - k::BALL_RADIUS;
            const double vin = dot(vIn[i], g.normal), vout = dot(b.state.velocity, g.normal);
            if (gap < 0.06 + (std::fabs(vin) + std::fabs(vout)) * h && vin < -0.8 && vout > vin + 0.4 && g.water <= g.height) {
                const double out = std::max(vout, 0.0);
                const double e = 0.5 * k::BALL_MASS * (vin * vin - out * out);
                if (e > 0.0) impacts.push_back({b.state.position - g.normal * double(k::BALL_RADIUS), g.normal, e, -vin, double(k::BALL_RADIUS)});
                b.state.squash = std::max(b.state.squash, std::min(double(k::BALL_SQUASH_MAX), -vin * double(k::BALL_SQUASH)));
                b.state.squashAxis = g.normal;
            }
        }
        plantContacts(b, before[i], b.state.position);
        // A safety net: should a ball ever end up under the ground, put it back on top.
        const double floor = groundUnder(b.state.position).height;
        if (b.state.position.y < floor - 0.5) {
            b.state.position.y = floor + k::BALL_RADIUS + 0.02;
            b.state.velocity = {b.state.velocity.x, std::max(b.state.velocity.y, 0.0), b.state.velocity.z};
            b.actor->setGlobalPose(PxTransform(toPx(b.state.position - origin), pose.q));
            b.actor->setLinearVelocity(toPx(b.state.velocity));
        }
    }
}

// ---------------------------------------------------------------------------------------------- the public face

Physics::Physics(const Terrain* terrain, const WindField* wind, uint64_t seed)
    : impl_(std::make_unique<Impl>(terrain, wind, seed)) {}

Physics::~Physics() = default;

uint32_t Physics::throwBall(const dvec3& from, const dvec3& velocity) {
    Impl& m = *impl_;
    if (!m.originSet) {
        m.origin = originNear(from);
        m.originSet = true;
    } else if (std::abs(from.x - m.origin.x) > kOriginReach || std::abs(from.z - m.origin.z) > kOriginReach) {
        const dvec3 o = originNear(from);
        m.scene->shiftOrigin(toPx(o - m.origin));
        m.origin = o;
    }
    while (!m.balls.empty() && int(m.balls.size()) >= k::BALL_MAX) m.removeBall(0);

    const double reach = 3.0 + std::hypot(velocity.x, velocity.z) * 0.35;
    m.streamGround(from, reach);
    m.streamTrees(from, reach + std::max(kTreeStream, m.treeReach));

    PxRigidDynamic* a = m.px->createRigidDynamic(PxTransform(toPx(from - m.origin)));
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*a, PxSphereGeometry(float(k::BALL_RADIUS)), *m.ballMaterial);
    shape->setContactOffset(0.02f);
    shape->setRestOffset(0.0f);
    a->setMass(float(k::BALL_MASS));
    // Between a solid ball (0.4 m r^2) and a hollow one (0.67 m r^2).
    a->setMassSpaceInertiaTensor(PxVec3(float(0.5 * k::BALL_MASS * k::BALL_RADIUS * k::BALL_RADIUS)));
    a->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, true);
    a->setLinearDamping(0.0f);
    a->setAngularDamping(0.02f);
    a->setSleepThreshold(0.0025f);
    a->setSolverIterationCounts(8, 2);
    a->setMaxDepenetrationVelocity(2.0f);
    m.scene->addActor(*a);
    a->setLinearVelocity(toPx(velocity));

    BallBody b;
    b.actor = a;
    b.state.id = m.nextId++;
    b.state.position = from;
    b.state.velocity = velocity;
    b.prevPosition = from;
    m.balls.push_back(std::move(b));
    m.view.clear();
    for (const BallBody& x : m.balls) m.view.push_back(x.state);
    return m.balls.back().state.id;
}

void Physics::addRock(uint32_t id, const dvec3& position, double radius) {
    Impl& m = *impl_;
    if (m.rocks.count(id)) return;
    if (!m.originSet) {
        m.origin = originNear(position);
        m.originSet = true;
    }
    PxRigidStatic* rock = m.px->createRigidStatic(PxTransform(toPx(position - m.origin)));
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*rock, PxSphereGeometry(float(radius)), *m.surfaces[kRock]);
    shape->setContactOffset(0.02f);
    shape->setRestOffset(0.0f);
    m.scene->addActor(*rock);
    m.rocks[id] = rock;
}

void Physics::addCapsule(uint32_t id, const dvec3& centre, double radius, double halfHeight) {
    Impl& m = *impl_;
    if (m.capsules.count(id)) return;
    if (!m.originSet) {
        m.origin = originNear(centre);
        m.originSet = true;
    }
    // PhysX capsules lie along x: stand it up. Kinematic: it moves with the body, nothing pushes it.
    PxRigidDynamic* body = m.px->createRigidDynamic(PxTransform(toPx(centre - m.origin), PxQuat(PxHalfPi, PxVec3(0.0f, 0.0f, 1.0f))));
    body->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*body, PxCapsuleGeometry(float(radius), float(halfHeight)), *m.bodyMaterial);
    shape->setContactOffset(0.02f);
    shape->setRestOffset(0.0f);
    m.scene->addActor(*body);
    m.capsules[id] = body;
}

void Physics::moveCapsule(uint32_t id, const dvec3& centre) {
    const auto it = impl_->capsules.find(id);
    if (it == impl_->capsules.end()) return;
    it->second->setKinematicTarget(PxTransform(toPx(centre - impl_->origin), PxQuat(PxHalfPi, PxVec3(0.0f, 0.0f, 1.0f))));
}

void Physics::removeRock(uint32_t id) {
    const auto it = impl_->rocks.find(id);
    if (it == impl_->rocks.end()) return;
    it->second->release();
    impl_->rocks.erase(it);
}

void Physics::setMagicSampler(MagicSampler sampler) { impl_->magic = std::move(sampler); }

void Physics::takeImpacts(std::vector<Impact>& out) {
    out.insert(out.end(), impl_->impacts.begin(), impl_->impacts.end());
    impl_->impacts.clear();
}

void Physics::groundChanged(double x0, double z0, double x1, double z1) {
    Impl& m = *impl_;
    for (auto it = m.tiles.begin(); it != m.tiles.end();) {
        const Tile& t = it->second;
        const double tx0 = double(it->first.first) * kTileSize, tz0 = double(it->first.second) * kTileSize;
        if (tx0 > x1 || tx0 + kTileSize < x0 || tz0 > z1 || tz0 + kTileSize < z0) {
            ++it;
            continue;
        }
        if (t.actor) t.actor->release();
        if (t.field) t.field->release();
        it = m.tiles.erase(it);
    }
    for (BallBody& b : m.balls) b.restKnown = false;
}

bool Physics::removeBall(uint32_t id) {
    Impl& m = *impl_;
    for (size_t i = 0; i < m.balls.size(); ++i)
        if (m.balls[i].state.id == id) {
            m.removeBall(i);
            m.view.clear();
            for (const BallBody& b : m.balls) m.view.push_back(b.state);
            return true;
        }
    return false;
}

void Physics::step(double dt, double seconds) {
    Impl& m = *impl_;
    ++m.stepIndex;
    m.tilesBuiltThisStep = 0;
    for (BallBody& b : m.balls) {
        b.prevPosition = b.state.position;
        std::copy(std::begin(b.state.rotation), std::end(b.state.rotation), std::begin(b.prevRotation));
    }
    if (!m.balls.empty()) {
        // Ground and trees where the balls are, and where they are about to be.
        for (const BallBody& b : m.balls) {
            const dvec3& v = b.state.velocity;
            const double reach = b.state.resting ? 1.0 : 3.0 + std::hypot(v.x, v.z) * 0.35 + std::max(-v.y, 0.0) * 0.1;
            m.streamGround(b.state.position, reach);
            m.streamTrees(b.state.position, (b.state.resting ? 0.5 : reach) + std::max(kTreeStream, m.treeReach));
        }
        const int n = std::max(1, int(k::PHYSICS_SUBSTEPS));
        const double h = dt / n;
        for (int s = 0; s < n; ++s) {
            m.seconds = seconds - dt + double(s + 1) * h;
            m.substep(h, s == 0);
        }
    }
    m.seconds = seconds;
    if (m.stepIndex % 60 == 0) {
        m.releaseUnusedGround();
        m.releaseUnusedTrees();
    }
    m.view.clear();
    for (const BallBody& b : m.balls) m.view.push_back(b.state);
}

const std::vector<Ball>& Physics::balls() const { return impl_->view; }

void Physics::poses(double alpha, std::vector<BallPose>& out) const {
    out.clear();
    const double t = clampd(alpha, 0.0, 1.0);
    for (const BallBody& b : impl_->balls) {
        BallPose p;
        p.id = b.state.id;
        p.position = b.prevPosition + (b.state.position - b.prevPosition) * t;
        // Normalized lerp along the shorter way.
        const double* q0 = b.prevRotation;
        const double* q1 = b.state.rotation;
        const double sign = q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3] < 0.0 ? -1.0 : 1.0;
        double q[4], len = 0.0;
        for (int i = 0; i < 4; ++i) {
            q[i] = q0[i] + (q1[i] * sign - q0[i]) * t;
            len += q[i] * q[i];
        }
        len = len > 0.0 ? 1.0 / std::sqrt(len) : 0.0;
        for (int i = 0; i < 4; ++i) p.rotation[i] = float(q[i] * len);
        p.squash = float(b.state.squash);
        p.squashAxis[0] = float(b.state.squashAxis.x);
        p.squashAxis[1] = float(b.state.squashAxis.y);
        p.squashAxis[2] = float(b.state.squashAxis.z);
        out.push_back(p);
    }
}

PhysicsStats Physics::stats() const {
    PhysicsStats s = impl_->stats;
    s.groundTiles = 0;
    for (const auto& [key, t] : impl_->tiles) s.groundTiles += t.actor ? 1 : 0;
    s.trees = int(impl_->trees.size());
    s.treeShapes = 0;
    for (const auto& [key, t] : impl_->trees) s.treeShapes += t.shapes;
    return s;
}

uint64_t Physics::stateHash() const {
    const Impl& m = *impl_;
    uint64_t h = hashCombine(uint64_t(m.stepIndex), m.nextId);
    uint64_t built = 0;
    for (const auto& [key, t] : m.tiles) built += t.actor ? 1 : 0;
    h = hashCombine(h, built);
    h = hashCombine(h, m.trees.size());
    h = hashCombine(h, m.stats.twigHits);
    h = hashCombine(h, m.stats.leafHits);
    for (const BallBody& b : m.balls) {
        h = hashCombine(h, b.state.id);
        h = hashCombine(h, (b.state.resting ? 1u : 0u) | (b.state.inWater ? 2u : 0u));
        const Ball& s = b.state;
        for (const double v : {s.position.x, s.position.y, s.position.z, s.velocity.x, s.velocity.y, s.velocity.z,
                               s.spin.x, s.spin.y, s.spin.z, s.rotation[0], s.rotation[1], s.rotation[2], s.rotation[3]})
            h = hashCombine(h, bits(v));
    }
    return h;
}

}  // namespace taelia
