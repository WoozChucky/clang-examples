#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>

#include <glm/glm.hpp>

#include "ECS.h"
#include "navigation/NavMesh.h"
#include "navigation/NavMeshBuilder.h"
#include "navigation/NavMeshSystem.h"
#include "navigation/NavServicesImpl.h"

#include "NavObstacleSync.h"
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

static EntityId SpawnNavBox(ECS& w, glm::vec3 pos, glm::vec3 halfExtents) {
    const EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{ pos, glm::vec3(0.0f), glm::vec3(1.0f) });
    ColliderComponent col{};
    col.Shape   = ColliderShape::Box;
    col.Size    = halfExtents;
    col.IsStatic = true;
    w.AddComponent(e, col);
    NavMeshSourceComponent src{};
    src.AreaId   = 63;
    src.Geometry = NavMeshGeometrySource::Collider;
    w.AddComponent(e, src);
    return e;
}

static NavMeshConfigComponent DefaultCfg() {
    return NavMeshConfigComponent{};
}

// Process-wide NavServices instance for tests that thread it through
// SystemContext. Lazy-initialized on first call so tests don't pay the
// init cost when they only test engine-internal NavMesh directly.
static const NavServices* TestNavServices() {
    static NavServices svc{};
    static bool initialized = false;
    if (!initialized) {
        NavServicesImpl::Init(svc);
        initialized = true;
    }
    return &svc;
}

static void T01_empty_world_yields_empty_navmesh() {
    ECS w;
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(!nm); // empty soup → null published
}

static void T02_flat_floor_path_is_straight() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(nm != nullptr);
    if (!nm) return;
    auto path = nm->FindPath(glm::vec3(-4, 0.5f, -4), glm::vec3(4, 0.5f, 4));
    EXPECT(path.size() >= 2);
}

static void T03_path_around_wall_is_curved() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    SpawnNavBox(w, glm::vec3(0,  1.0f, 0), glm::vec3(0.3f, 1.0f, 2.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(nm != nullptr);
    if (!nm) return;
    auto path = nm->FindPath(glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    EXPECT(path.size() > 2);
}

static void T04_unset_geometry_is_skipped() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    {
        const EntityId e = w.CreateEntity();
        w.AddComponent(e, TransformComponent{ glm::vec3(2, 0.5f, 0), glm::vec3(0), glm::vec3(1) });
        ColliderComponent c{};
        c.Shape = ColliderShape::Box;
        c.Size  = glm::vec3(0.5f);
        w.AddComponent(e, c);
        NavMeshSourceComponent src{};
        src.Geometry = NavMeshGeometrySource::Unset;
        w.AddComponent(e, src);
    }
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(nm != nullptr);
    if (nm) {
        auto path = nm->FindPath(glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
        EXPECT(path.size() >= 2);
    }
}

static void T05_mesh_geometry_without_meshcomponent_skipped() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    {
        const EntityId e = w.CreateEntity();
        w.AddComponent(e, TransformComponent{ glm::vec3(2, 0.5f, 0), glm::vec3(0), glm::vec3(1) });
        NavMeshSourceComponent src{};
        src.Geometry = NavMeshGeometrySource::Mesh;
        w.AddComponent(e, src);
    }
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(nm != nullptr);
    if (nm) {
        auto path = nm->FindPath(glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
        EXPECT(path.size() >= 2);
    }
}

static void T06_sphere_collider_routes_around() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    {
        const EntityId e = w.CreateEntity();
        w.AddComponent(e, TransformComponent{ glm::vec3(0, 1.0f, 0), glm::vec3(0), glm::vec3(1) });
        ColliderComponent c{};
        c.Shape = ColliderShape::Sphere;
        c.Size  = glm::vec3(1.5f);
        c.IsStatic = true;
        w.AddComponent(e, c);
        NavMeshSourceComponent src{};
        src.Geometry = NavMeshGeometrySource::Collider;
        w.AddComponent(e, src);
    }
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(nm != nullptr);
    if (nm) {
        auto path = nm->FindPath(glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
        EXPECT(path.size() > 2);
    }
}

static void T07_current_shared_across_threads() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto p1 = NavMeshSystem::Instance().Current();
    std::shared_ptr<const NavMesh> p2;
    std::thread t([&]{ p2 = NavMeshSystem::Instance().Current(); });
    t.join();
    EXPECT(p1 == p2);
}

static void T08_flat_floor_only_box_builds_walkable_navmesh() {
    // Regression: a single flat floor box (no walls, no ceiling — Y range = box thickness)
    // used to produce zero polys. Root cause: NavMeshBuilder::TriangulateBox emitted
    // INWARD-facing quad windings, so the +Y face had normal=-Y and
    // rcClearUnwalkableTriangles flagged it NULL (only the inverted -Y face passed as
    // "walkable"). With a thick box, the bottom-face span (walkable, voxel 0) sat just
    // below the top-face span (NULL, voxel 5), and rcFilterWalkableLowHeightSpans
    // killed it because the gap (4 voxels) was less than walkableHeight (9 voxels).
    // T02..T07 worked before because their boxes were 0.2 m thick (1 voxel) — the two
    // inverted face spans merged into one column and the bug was invisible.
    // NavMesh::Build also pads bmax.y by AgentHeight + 1 m as defence-in-depth for
    // future scenes with a low ceiling close to the floor.
    ECS w;
    SpawnNavBox(w, glm::vec3(0, 0, 0), glm::vec3(2.0f, 0.5f, 2.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(nm != nullptr);
    if (!nm) return;
    auto stats = nm->GetStats();
    EXPECT(stats.PolyCount > 0);   // would be 0 before the fix
    EXPECT(stats.TilesBuilt > 0);
    // Sanity: agent can path across the box top.
    auto path = nm->FindPath(glm::vec3(-1.5f, 1.0f, 0.0f), glm::vec3(1.5f, 1.0f, 0.0f));
    EXPECT(path.size() >= 2);
}

// Helper: drive dtTileCache::update until upToDate so tests see post-carve state.
// Bounded loop to avoid hangs on bugs.
static void DrainTileCache(NavMeshSystem& nav, int maxTicks = 16) {
    for (int i = 0; i < maxTicks; ++i) nav.Tick(0.016f);
}

// Shared helper for obstacle tests: floor + initial Rebuild that gives a usable navmesh.
static void SpawnFloorAndBuild(ECS& w) {
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
}

static EntityId SpawnCylinderObstacle(ECS& w, const glm::vec3& pos, float radius, float height) {
    const EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{ pos, glm::vec3(0.0f), glm::vec3(1.0f) });
    NavObstacleComponent obs{};
    obs.Shape = NavObstacleShape::Cylinder;
    obs.Size  = glm::vec3(radius, height, 0.0f);
    w.AddComponent(e, obs);
    return e;
}

static EntityId SpawnBoxObstacle(ECS& w, const glm::vec3& pos, const glm::vec3& halfExtents) {
    const EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{ pos, glm::vec3(0.0f), glm::vec3(1.0f) });
    NavObstacleComponent obs{};
    obs.Shape = NavObstacleShape::Box;
    obs.Size  = halfExtents;
    w.AddComponent(e, obs);
    return e;
}

static void RunSync(ECS& w, NavObstacleSyncSystem& sync) {
    SystemContext ctx{ w, 0.016, 0.0, TestNavServices() };
    sync.Update(ctx);
}

static void T09_cylinder_obstacle_blocks_path() {
    ECS w;
    SpawnFloorAndBuild(w);
    auto nm0 = NavMeshSystem::Instance().Current();
    EXPECT(nm0 != nullptr);
    auto pathBefore = nm0->FindPath(glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    EXPECT(pathBefore.size() == 2);  // straight line, no obstacle

    SpawnCylinderObstacle(w, glm::vec3(0, 0, 0), 1.5f, 2.0f);
    NavObstacleSyncSystem sync;
    RunSync(w, sync);
    DrainTileCache(NavMeshSystem::Instance());

    auto pathAfter = NavMeshSystem::Instance().Current()->FindPath(
        glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    EXPECT(pathAfter.size() > 2);  // forced to route around the cylinder
}

static void T10_box_obstacle_blocks_path() {
    ECS w;
    SpawnFloorAndBuild(w);
    SpawnBoxObstacle(w, glm::vec3(0, 1.0f, 0), glm::vec3(0.5f, 1.0f, 2.0f));  // wall-ish box
    NavObstacleSyncSystem sync;
    RunSync(w, sync);
    DrainTileCache(NavMeshSystem::Instance());

    auto path = NavMeshSystem::Instance().Current()->FindPath(
        glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    EXPECT(path.size() > 2);  // routes around the box
}

static void T11_remove_obstacle_restores_path() {
    ECS w;
    SpawnFloorAndBuild(w);
    const EntityId e = SpawnCylinderObstacle(w, glm::vec3(0, 0, 0), 1.5f, 2.0f);
    NavObstacleSyncSystem sync;

    RunSync(w, sync);
    DrainTileCache(NavMeshSystem::Instance());
    auto pathBlocked = NavMeshSystem::Instance().Current()->FindPath(
        glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    EXPECT(pathBlocked.size() > 2);

    w.RemoveComponent<NavObstacleComponent>(e);
    RunSync(w, sync);
    DrainTileCache(NavMeshSystem::Instance());
    auto pathRestored = NavMeshSystem::Instance().Current()->FindPath(
        glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    EXPECT(pathRestored.size() == 2);  // straight again
}

static void T12_obstacle_position_change_triggers_rebind() {
    ECS w;
    SpawnFloorAndBuild(w);
    const EntityId e = SpawnCylinderObstacle(w, glm::vec3(0, 0, 4.5f), 1.5f, 2.0f);  // off-path
    NavObstacleSyncSystem sync;

    RunSync(w, sync);
    DrainTileCache(NavMeshSystem::Instance());
    auto pathOffPath = NavMeshSystem::Instance().Current()->FindPath(
        glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    EXPECT(pathOffPath.size() == 2);  // obstacle is far from the line

    // Move obstacle onto the path.
    w.Modify<TransformComponent>(e, [](TransformComponent& t){ t.Position = glm::vec3(0, 0, 0); });
    RunSync(w, sync);
    DrainTileCache(NavMeshSystem::Instance());
    auto pathOnPath = NavMeshSystem::Instance().Current()->FindPath(
        glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    EXPECT(pathOnPath.size() > 2);
}

static void T13_rebuild_clears_obstacle_map() {
    ECS w;
    SpawnFloorAndBuild(w);
    const EntityId e = SpawnCylinderObstacle(w, glm::vec3(0, 0, 0), 1.5f, 2.0f);
    NavObstacleSyncSystem sync;

    RunSync(w, sync);
    DrainTileCache(NavMeshSystem::Instance());
    EXPECT(NavMeshSystem::Instance().ObstacleCount() == 1);
    (void)e;

    // Trigger Rebuild -- map clears + new navmesh, obstacle handle no longer tracked.
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    EXPECT(NavMeshSystem::Instance().ObstacleCount() == 0);

    // Next sync re-adds the obstacle (entity still has the component).
    RunSync(w, sync);
    EXPECT(NavMeshSystem::Instance().ObstacleCount() == 1);
}

// ---------- Spec 3: NavAgent helpers + tests ----------

static EntityId SpawnAgent(ECS& w, const glm::vec3& pos, float speed = 3.0f) {
    const EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{ pos, glm::vec3(0.0f), glm::vec3(1.0f) });
    NavAgentComponent agent{};
    agent.MoveSpeed = speed;
    w.AddComponent(e, agent);
    return e;
}

static void RunAgentSystem(ECS& w, NavAgentSystem& sys, double dt = 0.016) {
    SystemContext ctx{ w, dt, 0.0, TestNavServices() };
    sys.Update(ctx);
}

static void T14_agent_with_no_target_does_not_emit_intent() {
    ECS w;
    SpawnFloorAndBuild(w);
    SpawnAgent(w, glm::vec3(-2, 0.5f, 0));
    NavAgentSystem sys;
    RunAgentSystem(w, sys);
    // The Each gate requires NavTarget — no entity has MoveIntent.
    int intentCount = 0;
    w.Each<MoveIntentComponent>([&](EntityId){ ++intentCount; });
    EXPECT(intentCount == 0);
}

static void T15_agent_writes_intent_toward_path_waypoint() {
    ECS w;
    SpawnFloorAndBuild(w);
    const EntityId e = SpawnAgent(w, glm::vec3(-3, 0.5f, 0));
    w.AddComponent(e, NavTargetComponent{ glm::vec3(3, 0.5f, 0) });
    NavAgentSystem sys;
    RunAgentSystem(w, sys);

    auto* mi = w.GetComponent<MoveIntentComponent>(e);
    EXPECT(mi != nullptr);
    if (mi) {
        // Intent points roughly toward +X (target direction across open floor).
        EXPECT(mi->DesiredDelta.x > 0.0f);
        // Magnitude capped at MoveSpeed * dt = 3.0 * 0.016 = 0.048.
        const float mag = glm::length(mi->DesiredDelta);
        EXPECT(mag <= 3.0f * 0.016f + 1e-4f);
        EXPECT(mag > 0.0f);
    }
}

static void T16_agent_reached_target_stops_emitting() {
    ECS w;
    SpawnFloorAndBuild(w);
    const EntityId e = SpawnAgent(w, glm::vec3(0, 0.5f, 0));
    // Target within ReachedEpsilon (default 0.10) of position.
    w.AddComponent(e, NavTargetComponent{ glm::vec3(0.05f, 0.5f, 0) });
    NavAgentSystem sys;
    RunAgentSystem(w, sys);
    // No MoveIntent component should have been added (agent never emitted).
    EXPECT(!w.HasComponent<MoveIntentComponent>(e));
}

static void T17_agent_unreachable_target_no_intent() {
    ECS w;
    SpawnFloorAndBuild(w);
    const EntityId e = SpawnAgent(w, glm::vec3(0, 0.5f, 0));
    // Target way off the navmesh (far above floor).
    w.AddComponent(e, NavTargetComponent{ glm::vec3(0, 100.0f, 0) });
    NavAgentSystem sys;
    RunAgentSystem(w, sys);
    EXPECT(!w.HasComponent<MoveIntentComponent>(e));
}

static void T18_agent_routes_around_obstacle() {
    ECS w;
    SpawnFloorAndBuild(w);
    SpawnCylinderObstacle(w, glm::vec3(0, 0, 0), 1.5f, 2.0f);
    NavObstacleSyncSystem obsSync;
    RunSync(w, obsSync);
    DrainTileCache(NavMeshSystem::Instance());

    const EntityId e = SpawnAgent(w, glm::vec3(-3, 0.5f, 0));
    w.AddComponent(e, NavTargetComponent{ glm::vec3(3, 0.5f, 0) });
    NavAgentSystem sys;
    RunAgentSystem(w, sys);

    auto* mi = w.GetComponent<MoveIntentComponent>(e);
    EXPECT(mi != nullptr);
    if (mi) {
        // Path curves around the obstacle. First waypoint should NOT be straight
        // along +X (would mean walking into the cylinder). Z component should be
        // non-trivial (steering around).
        EXPECT(std::abs(mi->DesiredDelta.z) > 0.001f);
    }
}

// ---------- Spec 4: bake (T19-T22 file-format + load behavior) ----------

static std::string TempBakePath(const char* suffix = "test_navmesh_bake.bin") {
    auto tmp = std::filesystem::temp_directory_path() / suffix;
    return tmp.string();
}

static void T19_save_and_load_roundtrip_produces_equivalent_navmesh() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto built = NavMeshSystem::Instance().Current();
    EXPECT(built != nullptr);
    if (!built) return;

    const auto builtStats = built->GetStats();
    const std::string path = TempBakePath("test_navmesh_T19.bin");
    EXPECT(built->SaveToFile(path, /*worldMtime*/ 12345ULL));

    uint64_t storedMtime = 0;
    auto loaded = NavMesh::LoadFromFile(path, &storedMtime);
    EXPECT(loaded != nullptr);
    EXPECT(storedMtime == 12345ULL);
    if (!loaded) { std::filesystem::remove(path); return; }

    const auto loadedStats = loaded->GetStats();
    EXPECT(loadedStats.TilesBuilt == builtStats.TilesBuilt);
    EXPECT(loadedStats.PolyCount  == builtStats.PolyCount);
    EXPECT(loadedStats.VertCount  == builtStats.VertCount);

    // Smoke: FindPath on loaded navmesh works same as built.
    auto pathBuilt  = built->FindPath(glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    auto pathLoaded = loaded->FindPath(glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    EXPECT(pathBuilt.size() == pathLoaded.size());

    std::filesystem::remove(path);
}

static void T20_load_bad_magic_returns_null() {
    const std::string path = TempBakePath("test_navmesh_T20.bin");
    {
        std::ofstream ofs(path, std::ios::binary);
        const uint32_t badMagic = 0xDEADBEEF;
        ofs.write(reinterpret_cast<const char*>(&badMagic), 4);
    }
    uint64_t unused = 0;
    auto loaded = NavMesh::LoadFromFile(path, &unused);
    EXPECT(loaded == nullptr);
    std::filesystem::remove(path);
}

static void T21_load_bad_version_returns_null() {
    const std::string path = TempBakePath("test_navmesh_T21.bin");
    {
        std::ofstream ofs(path, std::ios::binary);
        const uint32_t kMagic = 0x484D534E;
        const uint32_t badVer = 999;
        ofs.write(reinterpret_cast<const char*>(&kMagic), 4);
        ofs.write(reinterpret_cast<const char*>(&badVer), 4);
    }
    uint64_t unused = 0;
    auto loaded = NavMesh::LoadFromFile(path, &unused);
    EXPECT(loaded == nullptr);
    std::filesystem::remove(path);
}

static void T22_load_missing_file_returns_null() {
    uint64_t unused = 0;
    auto loaded = NavMesh::LoadFromFile("Z:/definitely_does_not_exist_navmesh.bin", &unused);
    EXPECT(loaded == nullptr);
}

static void T23_try_load_from_disk_stale_returns_false() {
    namespace fs = std::filesystem;
    auto worldPath = fs::temp_directory_path() / "test_navmesh_T23_world.json";
    auto bakePath  = fs::temp_directory_path() / "test_navmesh_T23_world.navmesh.bin";

    // Initial world.json.
    { std::ofstream wf(worldPath); wf << "{}\n"; }

    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto built = NavMeshSystem::Instance().Current();
    EXPECT(built != nullptr);
    if (built) {
        // Save with a deliberately-stale worldMtime (much older than the file's actual mtime).
        EXPECT(built->SaveToFile(bakePath.string(), /*stale*/ 100ULL));
    }

    // Touch world.json forward so fs::mtime is well above stored 100.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    { std::ofstream wf(worldPath); wf << "{\"changed\":true}\n"; }

    const bool loaded = NavMeshSystem::Instance().TryLoadFromDisk(worldPath.string());
    EXPECT(!loaded);  // stale → returns false; caller falls back to Rebuild

    fs::remove(worldPath);
    fs::remove(bakePath);
}

// ---------- NavServices decoupling: T24-T25 ----------

static void T24_navservices_table_forwards_to_navmeshsystem() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);

    const NavServices* svc = TestNavServices();
    EXPECT(svc != nullptr);
    EXPECT(svc->HasMesh != nullptr);
    EXPECT(svc->FindPath != nullptr);
    EXPECT(svc->AddCylinderObstacle != nullptr);
    EXPECT(svc->HasMesh());

    std::vector<glm::vec3> path;
    svc->FindPath(glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0), 50.0f, &path);
    EXPECT(path.size() >= 2);

    const uint32_t h = svc->AddCylinderObstacle(glm::vec3(0, 0, 0), 1.0f, 2.0f);
    EXPECT(h != 0);
    svc->TrackObstacleForEntity(42, h);
    EXPECT(svc->FindObstacleForEntity(42) == h);
    svc->UntrackEntity(42);
    EXPECT(svc->FindObstacleForEntity(42) == 0);
    svc->RemoveObstacle(h);
}

static void T25_navservices_findpath_empty_when_no_mesh() {
    ECS empty;
    NavMeshSystem::Instance().Rebuild(empty, DefaultCfg(), nullptr);  // empty soup → null current

    const NavServices* svc = TestNavServices();
    EXPECT(!svc->HasMesh());

    std::vector<glm::vec3> path;
    svc->FindPath(glm::vec3(0), glm::vec3(1), 50.0f, &path);
    EXPECT(path.empty());
}

// ---------- Navigation Mesh Input: T26-T27 (Spec 5) ----------

static void T26_geometry_mesh_uses_cached_cpu_data() {
    ECS w;

    // Manually triangulated 4x4 floor (square at y=0, two triangles).
    // Field names match MeshVertex layout: px,py,pz, nx,ny,nz, u,v.
    std::vector<MeshVertex> verts = {
        {-2.0f, 0.0f, -2.0f,  0,0,1,  0,0},
        { 2.0f, 0.0f, -2.0f,  0,0,1,  1,0},
        { 2.0f, 0.0f,  2.0f,  0,0,1,  1,1},
        {-2.0f, 0.0f,  2.0f,  0,0,1,  0,1},
    };
    std::vector<uint32_t> indices = { 0, 1, 2, 0, 2, 3 };

    constexpr uint32_t kMeshId = 42;
    NavMeshSystem::Instance().StoreMeshCpuData(kMeshId, std::move(verts), std::move(indices));

    // Entity references the cached mesh via MeshComponent + NavMeshSource(Geometry=Mesh).
    const EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{ glm::vec3(0,0,0), glm::vec3(0), glm::vec3(1) });
    MeshComponent mc{};
    mc.MeshId = kMeshId;
    mc.Visible = true;
    w.AddComponent(e, mc);
    NavMeshSourceComponent src{};
    src.AreaId = 63;
    src.Geometry = NavMeshGeometrySource::Mesh;
    w.AddComponent(e, src);

    // Rebuild (no MeshSystem* — NavMeshBuilder consults the cache).
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg());

    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(nm != nullptr);
    if (!nm) return;
    EXPECT(nm->GetStats().PolyCount > 0);  // floor became walkable navmesh

    // FindPath across the floor returns 2-waypoint straight line.
    auto path = nm->FindPath(glm::vec3(-1.5f, 0.5f, 0), glm::vec3(1.5f, 0.5f, 0));
    EXPECT(path.size() >= 2);
}

static void T27_geometry_mesh_cache_miss_skips_entity() {
    ECS w;
    // Spawn a collider floor so navmesh isn't empty.
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));

    // Add a Mesh-source entity whose MeshId is NOT in the cache.
    const EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{ glm::vec3(2, 0.5f, 0), glm::vec3(0), glm::vec3(1) });
    MeshComponent mc{};
    mc.MeshId = 999;  // never stored in cache
    mc.Visible = true;
    w.AddComponent(e, mc);
    NavMeshSourceComponent src{};
    src.Geometry = NavMeshGeometrySource::Mesh;
    w.AddComponent(e, src);

    NavMeshSystem::Instance().Rebuild(w, DefaultCfg());

    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(nm != nullptr);  // floor still built; Mesh-source skipped + SM_WARN
}

int main() {
    T01_empty_world_yields_empty_navmesh();
    T02_flat_floor_path_is_straight();
    T03_path_around_wall_is_curved();
    T04_unset_geometry_is_skipped();
    T05_mesh_geometry_without_meshcomponent_skipped();
    T06_sphere_collider_routes_around();
    T07_current_shared_across_threads();
    T08_flat_floor_only_box_builds_walkable_navmesh();
    T09_cylinder_obstacle_blocks_path();
    T10_box_obstacle_blocks_path();
    T11_remove_obstacle_restores_path();
    T12_obstacle_position_change_triggers_rebind();
    T13_rebuild_clears_obstacle_map();
    T14_agent_with_no_target_does_not_emit_intent();
    T15_agent_writes_intent_toward_path_waypoint();
    T16_agent_reached_target_stops_emitting();
    T17_agent_unreachable_target_no_intent();
    T18_agent_routes_around_obstacle();
    T19_save_and_load_roundtrip_produces_equivalent_navmesh();
    T20_load_bad_magic_returns_null();
    T21_load_bad_version_returns_null();
    T22_load_missing_file_returns_null();
    T23_try_load_from_disk_stale_returns_false();
    T24_navservices_table_forwards_to_navmeshsystem();
    T25_navservices_findpath_empty_when_no_mesh();
    T26_geometry_mesh_uses_cached_cpu_data();
    T27_geometry_mesh_cache_miss_skips_entity();

    if (g_Failures) {
        std::fprintf(stderr, "%d test(s) failed.\n", g_Failures);
        return 1;
    }
    std::printf("All navmesh tests passed.\n");
    return 0;
}
