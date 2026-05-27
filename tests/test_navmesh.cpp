#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <thread>

#include <glm/glm.hpp>

#include "ECS.h"
#include "navigation/NavMesh.h"
#include "navigation/NavMeshBuilder.h"
#include "navigation/NavMeshSystem.h"

#include "NavObstacleSync.h"

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
    SystemContext ctx{ w, 0.016 };
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

    if (g_Failures) {
        std::fprintf(stderr, "%d test(s) failed.\n", g_Failures);
        return 1;
    }
    std::printf("All navmesh tests passed.\n");
    return 0;
}
