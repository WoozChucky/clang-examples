#include <cstdio>
#include <cstdlib>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtx/norm.hpp>

#include "ECS.h"
#include "Systems.h"
#include "navigation/NavMesh.h"
#include "navigation/NavMeshBuilder.h"
#include "navigation/NavMeshSystem.h"
#include "navigation/NavServicesImpl.h"

#include "NavAgentSystem.h"

void platform_debug_break(const char* expr, const char* file, int line, const char* message)
{
    std::fprintf(stderr, "ASSERT FAIL %s:%d: %s (expr: %s)\n",
                 (file ? file : "<unknown>"), line,
                 (message ? message : "<no message>"),
                 (expr ? expr : "<none>"));
    std::abort();
}

static int g_Failures = 0;
#define EXPECT(cond) do {                                              \
    if (!(cond)) {                                                     \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_Failures;                                                  \
    } } while (0)

// ---- Test infra ----

static int g_FindPathCallCount = 0;

// Snapshot of the real engine NavServices, captured in MakeCountingServices so the
// counting wrappers can delegate to the genuine implementation after overriding.
static NavServices g_RealServices{};

// Wrapped FindPath that records the call count for perf-assertion tests.
// Delegates to the real engine FindPath under the hood.
static void CountingFindPath(const glm::vec3& start, const glm::vec3& end,
                             float maxSearchRadius, std::vector<glm::vec3>* outPath) {
    ++g_FindPathCallCount;
    if (!outPath) return;
    outPath->clear();
    auto nm = NavMeshSystem::Instance().Current();
    if (!nm) return;
    const auto path = nm->FindPath(start, end, maxSearchRadius);
    outPath->reserve(path.size());
    for (const auto& pt : path) outPath->push_back(pt.Position);
}

// Wrapped FindPathForClass — this is the call NavAgentSystem actually makes
// (post the class-aware nav refactor), so it is the one that must be counted.
// Delegates to the real engine per-class query captured in g_RealServices.
static void CountingFindPathForClass(uint8_t classId, const glm::vec3& start, const glm::vec3& end,
                                     float maxSearchRadius, std::vector<glm::vec3>* outPath) {
    ++g_FindPathCallCount;
    if (g_RealServices.FindPathForClass)
        g_RealServices.FindPathForClass(classId, start, end, maxSearchRadius, outPath);
    else if (outPath)
        outPath->clear();
}

static NavServices MakeCountingServices() {
    NavServices svc{};
    NavServicesImpl::Init(svc);          // real forwarders for everything else
    g_RealServices = svc;                // snapshot real fn-ptrs for delegation
    svc.FindPath         = &CountingFindPath;          // legacy class-0 path (kept for completeness)
    svc.FindPathForClass = &CountingFindPathForClass;  // the path NavAgentSystem invokes — counted
    return svc;
}

static EntityId SpawnFloor(ECS& w, float halfX = 5.0f, float halfZ = 5.0f) {
    const EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{ glm::vec3(0, -0.1f, 0), glm::vec3(0.0f), glm::vec3(1.0f) });
    ColliderComponent col{};
    col.Shape   = ColliderShape::Box;
    col.Size    = glm::vec3(halfX, 0.1f, halfZ);
    col.IsStatic = true;
    w.AddComponent(e, col);
    NavMeshSourceComponent src{};
    src.AreaId   = 63;
    src.Geometry = NavMeshGeometrySource::Collider;
    w.AddComponent(e, src);
    return e;
}

static EntityId SpawnAgent(ECS& w, glm::vec3 pos, glm::vec3 dest) {
    const EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{ pos, glm::vec3(0.0f), glm::vec3(1.0f) });
    w.AddComponent(e, NavAgentComponent{});
    w.AddComponent(e, NavTargetComponent{ dest });
    return e;
}

static void TickOnce(ECS& w, NavAgentSystem& sys, const NavServices& svc, float dt = 1.0f/60.0f) {
    // SystemContext fields: {ECS&, double dt, double gameTime, const NavServices*}
    SystemContext ctx{ w, static_cast<double>(dt), 0.0, &svc };
    sys.Update(ctx);
}

// ---- Tests ----

static void T01_target_change_triggers_repath() {
    ECS w;
    SpawnFloor(w);
    NavMeshSystem::Instance().Rebuild(w, NavMeshConfigComponent{});

    const EntityId agent = SpawnAgent(w, glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    NavAgentSystem sys;
    const NavServices svc = MakeCountingServices();

    g_FindPathCallCount = 0;
    TickOnce(w, sys, svc);
    EXPECT(g_FindPathCallCount == 1);            // initial path cached

    // Change destination -> next tick triggers repath
    w.Modify<NavTargetComponent>(agent, [](NavTargetComponent& t){
        t.Destination = glm::vec3(4, 0.5f, 4);
    });
    TickOnce(w, sys, svc);
    EXPECT(g_FindPathCallCount == 2);            // target-change repath
}

static void T02_stable_state_no_repath() {
    ECS w;
    SpawnFloor(w);
    NavMeshSystem::Instance().Rebuild(w, NavMeshConfigComponent{});

    SpawnAgent(w, glm::vec3(-4, 0.5f, 0), glm::vec3(-3.5f, 0.5f, 0));  // very short path
    NavAgentSystem sys;
    const NavServices svc = MakeCountingServices();

    g_FindPathCallCount = 0;
    TickOnce(w, sys, svc);                       // initial repath
    const int afterInit = g_FindPathCallCount;
    EXPECT(afterInit == 1);

    // Hold target steady, no rebuild. Tick 30x (=0.5s @ 60Hz) — under kRepathInterval=1.0s.
    for (int i = 0; i < 30; ++i) TickOnce(w, sys, svc);
    EXPECT(g_FindPathCallCount == afterInit);    // no additional FindPath calls
}

static void T03_nav_rebuild_invalidates_path() {
    ECS w;
    SpawnFloor(w);
    NavMeshSystem::Instance().Rebuild(w, NavMeshConfigComponent{});

    SpawnAgent(w, glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    NavAgentSystem sys;
    const NavServices svc = MakeCountingServices();

    g_FindPathCallCount = 0;
    TickOnce(w, sys, svc);                       // initial repath
    EXPECT(g_FindPathCallCount == 1);

    // Rebuild bumps NavVersion -> next tick repath
    NavMeshSystem::Instance().Rebuild(w, NavMeshConfigComponent{});
    TickOnce(w, sys, svc);
    EXPECT(g_FindPathCallCount == 2);
}

static void T04_strayed_agent_repaths() {
    ECS w;
    SpawnFloor(w);
    NavMeshSystem::Instance().Rebuild(w, NavMeshConfigComponent{});

    const EntityId agent = SpawnAgent(w, glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    NavAgentSystem sys;
    const NavServices svc = MakeCountingServices();

    g_FindPathCallCount = 0;
    TickOnce(w, sys, svc);                       // initial repath; path along +X
    EXPECT(g_FindPathCallCount == 1);

    // Teleport agent far off the path segment (+Z direction, way beyond 2*Radius=1.0)
    w.Modify<TransformComponent>(agent, [](TransformComponent& tr){
        tr.Position = glm::vec3(-4, 0.5f, 3);
    });
    TickOnce(w, sys, svc);
    EXPECT(g_FindPathCallCount == 2);            // stray repath
}

static void T05_safety_timer_triggers_repath() {
    ECS w;
    SpawnFloor(w);
    NavMeshSystem::Instance().Rebuild(w, NavMeshConfigComponent{});

    SpawnAgent(w, glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));  // long path; agent is stationary (no system moves it) so safety timer can elapse
    NavAgentSystem sys;
    const NavServices svc = MakeCountingServices();

    g_FindPathCallCount = 0;
    TickOnce(w, sys, svc);
    EXPECT(g_FindPathCallCount == 1);

    // 70 ticks at 1/60s = ~1.17s — exceeds kRepathInterval=1.0s
    for (int i = 0; i < 70; ++i) TickOnce(w, sys, svc);
    EXPECT(g_FindPathCallCount >= 2);            // safety timer fired at least once
}

static void T06_path_cap_truncation_and_reflow() {
    // Use a long floor + far destination to force a long (potentially capped) path.
    ECS w;
    const EntityId floor = w.CreateEntity();
    w.AddComponent(floor, TransformComponent{ glm::vec3(0, -0.1f, 0), glm::vec3(0.0f), glm::vec3(1.0f) });
    ColliderComponent col{};
    col.Shape = ColliderShape::Box;
    col.Size  = glm::vec3(50.0f, 0.1f, 50.0f);
    col.IsStatic = true;
    w.AddComponent(floor, col);
    NavMeshSourceComponent src{};
    src.AreaId = 63; src.Geometry = NavMeshGeometrySource::Collider;
    w.AddComponent(floor, src);
    NavMeshSystem::Instance().Rebuild(w, NavMeshConfigComponent{});

    const EntityId agent = SpawnAgent(w, glm::vec3(-45, 0.5f, 0), glm::vec3(45, 0.5f, 0));
    NavAgentSystem sys;
    const NavServices svc = MakeCountingServices();

    TickOnce(w, sys, svc);

    // Verify PathCount in (0, kMaxPathPoints]. Use the returned EntityId — the
    // ECS reserves entity id 1 for its singleton, so positional ids drift.
    auto* a = w.GetComponent<NavAgentComponent>(agent);
    EXPECT(a != nullptr);
    if (!a) return;
    EXPECT(a->PathCount > 0);
    EXPECT(a->PathCount <= NavAgentComponent::kMaxPathPoints);
}

static void T07_reached_destination_early_returns() {
    ECS w;
    SpawnFloor(w);
    NavMeshSystem::Instance().Rebuild(w, NavMeshConfigComponent{});

    // Agent spawned inside ReachedEpsilon of target.
    const EntityId agent = SpawnAgent(w, glm::vec3(0, 0.5f, 0), glm::vec3(0.05f, 0.5f, 0));
    NavAgentSystem sys;
    const NavServices svc = MakeCountingServices();

    g_FindPathCallCount = 0;
    TickOnce(w, sys, svc);

    EXPECT(g_FindPathCallCount == 0);  // reached check returns before any FindPath
    EXPECT(!w.HasComponent<MoveIntentComponent>(agent));  // no MoveIntent emitted
}

static void T08_path_index_advances_on_waypoint_reach() {
    ECS w;
    SpawnFloor(w);
    NavMeshSystem::Instance().Rebuild(w, NavMeshConfigComponent{});

    const EntityId agent = SpawnAgent(w, glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    NavAgentSystem sys;
    const NavServices svc = MakeCountingServices();

    TickOnce(w, sys, svc);

    auto* a0 = w.GetComponent<NavAgentComponent>(agent);
    EXPECT(a0 != nullptr);
    if (!a0 || a0->PathCount < 2) return;
    const uint8_t initialIndex = a0->PathIndex;
    const glm::vec3 waypoint = a0->CachedPath[initialIndex];

    // Teleport agent within ReachedEpsilon of CachedPath[initialIndex] -> PathIndex advances next tick.
    w.Modify<TransformComponent>(agent, [&](TransformComponent& tr){
        tr.Position = waypoint;  // exactly on waypoint -> dist < ReachedEpsilon
    });
    TickOnce(w, sys, svc);
    auto* a1 = w.GetComponent<NavAgentComponent>(agent);
    EXPECT(a1 != nullptr);
    if (!a1) return;
    EXPECT(a1->PathIndex == initialIndex + 1);
}

static void T09_cached_path_visible_off_snapshot() {
    ECS w;
    SpawnFloor(w);
    NavMeshSystem::Instance().Rebuild(w, NavMeshConfigComponent{});

    const EntityId agent = SpawnAgent(w, glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    NavAgentSystem sys;
    const NavServices svc = MakeCountingServices();

    TickOnce(w, sys, svc);

    // Snapshot the world; read CachedPath off the snapshot.
    auto snap = w.CreateSnapshot();
    EXPECT(snap != nullptr);
    if (!snap) return;
    const auto* snapAgent = snap->GetComponent<NavAgentComponent>(agent);
    EXPECT(snapAgent != nullptr);
    if (!snapAgent) return;

    const auto* masterAgent = w.GetComponent<NavAgentComponent>(agent);
    EXPECT(masterAgent != nullptr);
    if (!masterAgent) return;

    EXPECT(snapAgent->PathCount == masterAgent->PathCount);
    for (uint8_t i = 0; i < snapAgent->PathCount; ++i) {
        EXPECT(snapAgent->CachedPath[i] == masterAgent->CachedPath[i]);
    }
}

int main() {
    T01_target_change_triggers_repath();
    T02_stable_state_no_repath();
    T03_nav_rebuild_invalidates_path();
    T04_strayed_agent_repaths();
    T05_safety_timer_triggers_repath();
    T06_path_cap_truncation_and_reflow();
    T07_reached_destination_early_returns();
    T08_path_index_advances_on_waypoint_reach();
    T09_cached_path_visible_off_snapshot();

    if (g_Failures == 0) {
        std::printf("All NavAgent tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "NavAgent tests: %d failure(s)\n", g_Failures);
    return 1;
}
