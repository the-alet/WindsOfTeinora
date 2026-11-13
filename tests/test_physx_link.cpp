// PhysX links and runs: a ball dropped on a plane comes to rest on it, the same way every time.
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#include "PxPhysicsAPI.h"
#include "test.h"

using namespace physx;

namespace {

// Counts what PhysX allocates, so a leak shows up as a non-zero balance after release.
class CountingAllocator : public PxAllocatorCallback {
public:
    void* allocate(size_t size, const char*, const char*, int) override {
        void* p = _aligned_malloc(size, 16);
        if (p) ++live;
        return p;
    }
    void deallocate(void* ptr) override {
        if (ptr) --live;
        _aligned_free(ptr);
    }
    std::atomic<int> live{0};
};

class CountingErrors : public PxErrorCallback {
public:
    void reportError(PxErrorCode::Enum code, const char* message, const char* file, int line) override {
        if (code == PxErrorCode::eDEBUG_INFO) return;
        ++count;
        std::printf("  PhysX error %d: %s (%s:%d)\n", int(code), message, file, line);
    }
    int count = 0;
};

struct DropResult {
    PxVec3 pos{0.0f};
    int leaked = -1;
    int errors = -1;
};

DropResult dropBall() {
    CountingAllocator alloc;
    CountingErrors errors;
    DropResult r;
    PxFoundation* foundation = PxCreateFoundation(PX_PHYSICS_VERSION, alloc, errors);
    if (!foundation) return r;
    PxPhysics* physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, PxTolerancesScale());
    PxDefaultCpuDispatcher* dispatcher = PxDefaultCpuDispatcherCreate(0);

    PxSceneDesc desc(physics->getTolerancesScale());
    desc.gravity = PxVec3(0.0f, -9.81f, 0.0f);
    desc.cpuDispatcher = dispatcher;
    desc.filterShader = PxDefaultSimulationFilterShader;
    desc.flags |= PxSceneFlag::eENABLE_ENHANCED_DETERMINISM;
    PxScene* scene = physics->createScene(desc);

    PxMaterial* material = physics->createMaterial(0.6f, 0.5f, 0.0f);
    PxRigidStatic* ground = PxCreatePlane(*physics, PxPlane(0.0f, 1.0f, 0.0f, 0.0f), *material);
    PxRigidDynamic* ball = PxCreateDynamic(*physics, PxTransform(PxVec3(0.0f, 2.0f, 0.0f)), PxSphereGeometry(0.1f), *material, 1000.0f);
    scene->addActor(*ground);
    scene->addActor(*ball);

    for (int i = 0; i < 180; ++i) {
        scene->simulate(1.0f / 60.0f);
        scene->fetchResults(true);
    }
    r.pos = ball->getGlobalPose().p;

    ball->release();
    ground->release();
    material->release();
    scene->release();
    dispatcher->release();
    physics->release();
    foundation->release();
    r.leaked = alloc.live.load();
    r.errors = errors.count;
    return r;
}

}  // namespace

TEST(physx_ball_rests_on_the_ground_the_same_way_twice) {
    const DropResult a = dropBall();
    const DropResult b = dropBall();
    CHECK_NEAR(a.pos.y, 0.1, 0.01);
    CHECK_NEAR(a.pos.x, 0.0, 1e-6);
    CHECK_NEAR(a.pos.z, 0.0, 1e-6);
    CHECK(std::memcmp(&a.pos, &b.pos, sizeof(PxVec3)) == 0);  // bit-identical
    CHECK(a.leaked == 0 && b.leaked == 0);
    CHECK(a.errors == 0 && b.errors == 0);
}
