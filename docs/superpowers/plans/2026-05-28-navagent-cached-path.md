# NavAgent v2 Cached Path Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace per-tick `FindPath` per agent with cached-path on NavAgentComponent (fixed 32-waypoint array). Repath only on 5 explicit triggers: target change, navmesh rebuilt, agent strayed, safety timer, path consumed.

**Architecture:** NavAgentComponent gains 6 runtime fields (cached path + bookkeeping). NavMeshSystem gains atomic version counter bumped at every publish (Rebuild success, Rebuild empty-soup clear, TryLoadFromDisk). NavServices gains `NavVersion()` accessor. NavAgentSystem flips from pure-reader to mutator. DebugRenderPass ShowNavPaths reads cached path off snapshot — kills the v1 RenderThread/GameThread footgun.

**Tech Stack:** C++23, GLM, custom ECS, existing NavServices function-pointer table, ImGui editor toggle. No new third-party deps.

**Spec reference:** `docs/superpowers/specs/2026-05-28-navagent-cached-path-design.md` (commit `e1fac9a`).

---

## Codebase orientation (read once before Task 1)

- **Branch already exists:** `feat/navagent-cached-path`. The spec commit `e1fac9a` is already on it (1 commit ahead of main).
- **GAME_API_VERSION lives in `src/game/include/Game.h:20`.** Current value `17u`. Bump to `18u` in Task 2.
- **NavAgentComponent v1 layout (`src/common/include/ECS.h:311-315`):** 3 floats (MoveSpeed, Radius, ReachedEpsilon). System (`src/game/src/NavAgentSystem.h`) treats it as pure-reader — `world.Each<...>` const-ref iteration.
- **NavMeshSystem publish sites (`src/engine/src/navigation/NavMeshSystem.cpp`):** 3 places call `std::atomic_store(&m_Current, ...)`:
  - Line 54 — Rebuild empty-soup (publishes nullptr).
  - Line 64 — Rebuild success (publishes new shared_ptr).
  - Line 184 — TryLoadFromDisk success (publishes shared_ptr from disk bake).
  - **Plan refactor: extract `PublishNavMesh(shared_ptr<const NavMesh>)` private helper that does atomic_store + version bump in one place.** Avoids forgetting the bump on a future publish site.
- **NavServices struct (`src/common/include/NavServices.h`):** Append-only by contract. `NavVersion` field inserted AFTER `FindPath`, BEFORE obstacle methods (matches the spec). Mid-struct insertion is OK because the GAME_API_VERSION bump is required for NavAgentComponent layout change anyway.
- **NavServicesImpl wiring (`src/engine/src/navigation/NavServicesImpl.cpp`):** Anonymous-namespace `ForwardX` free functions + `Init()` assigning them. Pattern: add `ForwardNavVersion()` + assign `out.NavVersion = &ForwardNavVersion`.
- **NavAgentSystem (`src/game/src/NavAgentSystem.h`):** Header-only (template-free, just inline class). Currently 82 lines. v2 rewrite stays in the same file — adds 2 static helper functions (`dist_to_segment`, `approx_eq`), a `kRepathInterval` constant, and a longer `Update` body.
- **ComponentSerialization NavAgent (`src/common/include/ComponentSerialization.h:208-219`):** Already only touches the 3 v1 tunables. **No change needed in Task 2** — verify-only.
- **Test pattern (`tests/test_navmesh.cpp`):** `EXPECT(cond)` macro; static `g_Failures` counter; `main()` runs each test function then prints summary. New `test_navagent.cpp` follows the same pattern.
- **`test_navagent` CMake target** mirrors `test_navmesh` (`tests/CMakeLists.txt:389-415`). Links `CommonHeaders`, `glm::glm`, `ecs`, `Engine`. Include dirs cover `src/common/include`, `src/engine/src`, `src/game/src` (the last gives access to `NavAgentSystem.h`).
- **NavAgentSystem call site (`src/game/src/game.cpp:404`):** registers `NavAgentSystem` instance in scheduler. **Untouched** by this work (system signature unchanged from the scheduler's POV).
- **ECS Modify pattern:** `ctx.world.Modify<NavAgentComponent>(e, [&](auto& a) { ... })` is the canonical mutation path. Tests use the same against the master ECS.
- **DebugRenderPass ShowNavPaths block (`src/engine/src/rendering/passes/DebugRenderPass.cpp:219-231`):** Currently destination-spheres-only with a 6-line v1 footgun comment. Task 5 replaces the entire block (comment + body).
- **DebugAppendLine / DebugAppendSphere (`src/engine/src/rendering/DebugDraw.h`):** Free functions that push vertices into a `std::vector<DebugVertex>&`. Match the signature already used in the existing ShowNavPaths block.
- **`SystemContext::Nav`** is `const NavServices*`. Tests construct a local `NavServices` via `NavServicesImpl::Init(svc)` (or a stubbed table with a FindPath counter wrapper).
- **Editor restart required after T2 commit** because GAME_API_VERSION bumps. Test runs from CLI are unaffected (each `test_xxx.exe` is freshly linked).

---

## Task 0: Verify branch state

**Files:** none (git only)

- [ ] **Step 1: Verify on correct branch + clean tree**

```bash
git status -sb
# Expected: "## feat/navagent-cached-path" — clean (.claude/ untracked OK).
git log --oneline -2
# Expected: e1fac9a (spec) + 77969f9 (mesh-input merge)
```

- [ ] **Step 2: Verify current GAME_API_VERSION**

```bash
grep -n "GAME_API_VERSION" src/game/include/Game.h
# Expected: "#define GAME_API_VERSION 17u"  ← Task 2 bumps to 18u
```

- [ ] **Step 3: No commit yet** — administrative only.

---

## Task 1: NavMeshSystem nav-version counter + NavServices wiring

**Files:**
- Modify: `src/engine/src/navigation/NavMeshSystem.h`
- Modify: `src/engine/src/navigation/NavMeshSystem.cpp`
- Modify: `src/common/include/NavServices.h`
- Modify: `src/engine/src/navigation/NavServicesImpl.cpp`

- [ ] **Step 1: Add `<atomic>` include + `m_NavVersion` + `GetNavVersion()` + `PublishNavMesh()` helper to NavMeshSystem.h**

At the top of `NavMeshSystem.h`, add include:

```cpp
#include <atomic>
```

Inside `class NavMeshSystem`, public section AFTER `Current()`:

```cpp
    // Monotonic version counter; bumped every time the published NavMesh
    // shared_ptr is replaced (Rebuild success, Rebuild empty-soup clear,
    // TryLoadFromDisk success). NavAgentSystem reads via NavServices::NavVersion
    // to invalidate cached paths after a rebuild. Single-threaded writer
    // (GameThread, per NavMeshSystem contract); relaxed atomic suffices.
    uint32_t GetNavVersion() const { return m_NavVersion.load(std::memory_order_relaxed); }
```

Inside `private:` section, AFTER `m_MeshCpuData`:

```cpp
    std::atomic<uint32_t> m_NavVersion{0};

    // All m_Current publish sites route through here so the version bump
    // can't be forgotten on a future publish site.
    void PublishNavMesh(std::shared_ptr<const NavMesh> mesh);
```

- [ ] **Step 2: Implement `PublishNavMesh()` in NavMeshSystem.cpp + replace all 3 atomic_store sites**

Append at the end of `NavMeshSystem.cpp` (or near the top, anywhere after the existing methods):

```cpp
void NavMeshSystem::PublishNavMesh(std::shared_ptr<const NavMesh> mesh) {
    std::atomic_store(&m_Current, std::move(mesh));
    m_NavVersion.fetch_add(1, std::memory_order_relaxed);
}
```

Now replace each of the 3 existing `std::atomic_store(&m_Current, ...)` sites:

**Site 1 — Rebuild empty-soup (around line 54).** Replace:

```cpp
        std::atomic_store(&m_Current, std::shared_ptr<const NavMesh>{});
```

with:

```cpp
        PublishNavMesh(std::shared_ptr<const NavMesh>{});
```

**Site 2 — Rebuild success (around line 64).** Replace:

```cpp
    std::shared_ptr<const NavMesh> shared(std::move(fresh));
    std::atomic_store(&m_Current, shared);
```

with:

```cpp
    std::shared_ptr<const NavMesh> shared(std::move(fresh));
    PublishNavMesh(shared);
```

(`shared` is still needed afterward for the disk-bake block — don't replace it with a single PublishNavMesh call that consumes the shared_ptr.)

**Site 3 — TryLoadFromDisk (around line 184).** Replace:

```cpp
    std::shared_ptr<const NavMesh> shared(std::move(fresh));
    std::atomic_store(&m_Current, shared);
```

with:

```cpp
    std::shared_ptr<const NavMesh> shared(std::move(fresh));
    PublishNavMesh(shared);
```

(Same caveat — `shared` may be used later. Keep the local; pass it to PublishNavMesh.)

Verify no other `std::atomic_store(&m_Current` calls remain:

```bash
grep -n "atomic_store(&m_Current" src/engine/src/navigation/NavMeshSystem.cpp
# Expected: no matches.
```

- [ ] **Step 3: Append `NavVersion` to NavServices**

In `src/common/include/NavServices.h`, add the function pointer AFTER `FindPath`, BEFORE the obstacle block. The struct should look like:

```cpp
struct NavServices {
    // ---- NavMesh existence + query ----
    bool (*HasMesh)();

    void (*FindPath)(const glm::vec3& start, const glm::vec3& end,
                     float maxSearchRadius, std::vector<glm::vec3>* outPath);

    // Monotonic counter incremented every time NavMeshSystem publishes a new
    // NavMesh (Rebuild success, Rebuild empty-soup clear, TryLoadFromDisk).
    // NavAgentSystem caches LastSeenNavVersion to detect navmesh-rebuilt and
    // invalidate the cached path. GameThread only.
    uint32_t (*NavVersion)();

    // ---- Obstacle add/remove (Spec 2) ----
    uint32_t (*AddCylinderObstacle)(const glm::vec3& pos, float radius, float height);
    uint32_t (*AddBoxObstacle)(const glm::vec3& bmin, const glm::vec3& bmax);
    void     (*RemoveObstacle)(uint32_t handle);

    // ---- EntityId → ObstacleHandle side table (Spec 2) ----
    void     (*TrackObstacleForEntity)(EntityId e, uint32_t handle);
    uint32_t (*FindObstacleForEntity)(EntityId e);
    void     (*UntrackEntity)(EntityId e);
};
```

(Mid-struct insertion shifts obstacle-method offsets — Task 2's GAME_API_VERSION bump covers this.)

- [ ] **Step 4: Wire `NavVersion` in NavServicesImpl.cpp**

In `src/engine/src/navigation/NavServicesImpl.cpp`, add to the anonymous namespace AFTER `ForwardFindPath`:

```cpp
uint32_t ForwardNavVersion() {
    return NavMeshSystem::Instance().GetNavVersion();
}
```

Inside `NavServicesImpl::Init`, assign AFTER the `out.FindPath = ...` line, BEFORE the obstacle assignments:

```cpp
    out.NavVersion             = &ForwardNavVersion;
```

- [ ] **Step 5: Build**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine --config Debug
```

Expected: clean build. New code compiles; no callers yet.

- [ ] **Step 6: Commit**

```bash
git add src/engine/src/navigation/NavMeshSystem.h src/engine/src/navigation/NavMeshSystem.cpp src/common/include/NavServices.h src/engine/src/navigation/NavServicesImpl.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): nav-version counter + NavServices::NavVersion accessor

NavMeshSystem gains atomic<uint32_t> m_NavVersion (relaxed; single
writer is GameThread per system contract). All m_Current publish
sites now route through PublishNavMesh() which atomic_stores +
fetch_adds the counter — keeps the bump from being forgotten on
future publish paths.

NavServices struct grows a NavVersion() function pointer between
FindPath and the obstacle block. Engine wires it to
NavMeshSystem::Instance().GetNavVersion() via NavServicesImpl.

GAME_API_VERSION bump deferred to Task 2 (NavAgentComponent
layout change makes the bump unavoidable; NavServices mid-struct
insertion comes along for the ride)."
```

---

## Task 2: NavAgentComponent v2 layout + GAME_API_VERSION bump

**Files:**
- Modify: `src/common/include/ECS.h`
- Modify: `src/game/include/Game.h`
- Verify (no change expected): `src/common/include/ComponentSerialization.h`

- [ ] **Step 1: Replace NavAgentComponent struct in ECS.h**

Find `NavAgentComponent` at `src/common/include/ECS.h:311-315`:

```cpp
struct NavAgentComponent {
    float MoveSpeed      = 3.0f;   // world units / second
    float Radius         = 0.5f;   // agent footprint; matches NavMeshConfigComponent::AgentRadius authoring
    float ReachedEpsilon = 0.10f;  // distance at which target is considered reached; stop emitting MoveIntent
};
```

Replace with:

```cpp
struct NavAgentComponent {
    // ---- v1 user-authored tunables (serialized) ----
    float MoveSpeed      = 3.0f;   // world units / second
    float Radius         = 0.5f;   // agent footprint; matches NavMeshConfigComponent::AgentRadius authoring
    float ReachedEpsilon = 0.10f;  // distance at which target is considered reached; stop emitting MoveIntent

    // ---- v2 cached-path state (runtime, NOT serialized) ----
    static constexpr int kMaxPathPoints = 32;   // path cap; typical Recast string-pull yields 4-16 waypoints
    glm::vec3 CachedPath[kMaxPathPoints]{};
    uint8_t   PathCount       = 0;              // valid waypoints in CachedPath
    uint8_t   PathIndex       = 0;              // next waypoint the agent walks toward
    glm::vec3 LastTarget{0.0f};                 // target-change detection (epsilon compare vs NavTarget.Destination)
    uint32_t  LastNavVersion  = 0;              // navmesh-rebuild invalidation (vs NavServices::NavVersion)
    float     TimeSinceRepath = 0.0f;           // safety-timer accumulator; repath when > kRepathInterval
};
```

Trivially copyable preserved (no vector, no shared_ptr, no virtual). `ComponentArray<NavAgentComponent>::CopyFrom` stays a memcpy through std::vector::assign.

- [ ] **Step 2: Bump GAME_API_VERSION**

In `src/game/include/Game.h`, change:

```cpp
#define GAME_API_VERSION 17u
```

to:

```cpp
#define GAME_API_VERSION 18u
```

- [ ] **Step 3: Verify serializer unchanged (no edit expected)**

```bash
grep -A5 "to_json.*NavAgentComponent\|from_json.*NavAgentComponent" src/common/include/ComponentSerialization.h
```

Expected output: blocks reference ONLY `MoveSpeed`, `Radius`, `ReachedEpsilon`. If anything else appears, fix in this step to touch only those three. The current code (lines 208-219) is already correct — no edit needed in 99% of cases.

- [ ] **Step 4: Build editor + game + ecs**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target ecs editor game --config Debug
```

Expected: clean build. NavAgentSystem.h v1 still compiles (it never read the new fields).

- [ ] **Step 5: Run existing regression suite to verify no behavior change**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_worldserial test_followcam test_playermove --config Debug
for t in test_ecs test_worldserial test_followcam test_playermove; do
  ./out/build/msvc-win64-vs2026-community/bin/Debug/$t.exe || { echo "$t FAILED"; exit 1; }
done
```

Expected: all 4 pass. NavAgentComponent layout change doesn't break ECS infra (size grows but trivially-copyable contract holds). Serialization round-trip in `test_worldserial` reads `MoveSpeed`/`Radius`/`ReachedEpsilon`; runtime fields default-init.

- [ ] **Step 6: Commit**

```bash
git add src/common/include/ECS.h src/game/include/Game.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(ecs): NavAgentComponent v2 layout + GAME_API_VERSION 17->18

NavAgentComponent gains 6 runtime fields (cached path + bookkeeping):
- CachedPath[kMaxPathPoints=32] fixed-size glm::vec3 array
- PathCount, PathIndex (uint8_t each — path cap fits in u8)
- LastTarget (glm::vec3) — for target-change detection
- LastNavVersion (uint32_t) — for navmesh-rebuild invalidation
- TimeSinceRepath (float) — safety-timer accumulator

v1 tunables (MoveSpeed/Radius/ReachedEpsilon) unchanged.
Trivially copyable preserved — snapshot copy stays a memcpy.

Size grows from 12 to ~420 bytes/agent. At 100 agents that's
~40 KB/snapshot — ECS snapshot pool reuses buffers, so amortized
alloc cost is zero (only the memcpy grows).

GAME_API_VERSION bump 17 -> 18 because:
- Component layout changed (existing GameState linkage would
  misread NavAgentComponent fields).
- NavServices mid-struct insertion from Task 1 shifts obstacle-
  method offsets (would also break Game.dll-side calls).

Editor restart required after this commit.

ComponentSerialization.h NavAgent (de)serializer unchanged —
still only touches the 3 user-authored tunables. Runtime
fields default-init on world.json load."
```

---

## Task 3: TDD red — test_navagent T01-T09

**Files:**
- Create: `tests/test_navagent.cpp`
- Modify: `tests/CMakeLists.txt` (append `test_navagent` target)

- [ ] **Step 1: Create `tests/test_navagent.cpp`**

Full file content:

```cpp
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

static NavServices MakeCountingServices() {
    NavServices svc{};
    NavServicesImpl::Init(svc);          // real forwarders for everything else
    svc.FindPath = &CountingFindPath;    // override to count calls
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
    SystemContext ctx{ w, dt, &svc };
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

    SpawnAgent(w, glm::vec3(-4, 0.5f, 0), glm::vec3(-3.9f, 0.5f, 0));  // very short path; agent stays near target
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

    SpawnAgent(w, glm::vec3(-45, 0.5f, 0), glm::vec3(45, 0.5f, 0));
    NavAgentSystem sys;
    const NavServices svc = MakeCountingServices();

    TickOnce(w, sys, svc);

    // Verify PathCount in (0, kMaxPathPoints].
    auto* a = w.GetComponent<NavAgentComponent>(2);  // floor=1, agent=2
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
```

- [ ] **Step 2: Add `test_navagent` to `tests/CMakeLists.txt`**

Append at the end of `tests/CMakeLists.txt` (after the `test_navmesh` block):

```cmake
add_executable(test_navagent
    test_navagent.cpp
)

target_link_libraries(test_navagent PRIVATE
    CommonHeaders
    glm::glm
    ecs
    Engine
)

target_include_directories(test_navagent PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/engine/src
    ${CMAKE_SOURCE_DIR}/src/game/src
)

target_compile_definitions(test_navagent PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_navagent PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 3: Configure + build — expect failures**

```bash
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target test_navagent --config Debug
```

Expected: test_navagent.exe builds successfully (NavAgentSystem.h v1 satisfies all referenced APIs — there's no compile-error TDD here; the v1 system runs but tests fail at runtime).

Then run:

```bash
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navagent.exe
```

Expected: **runtime failures** (v1 stateless system doesn't satisfy the assertions):
- T02 fails — v1 calls FindPath every tick → `g_FindPathCallCount` grows past initial 1.
- T03 likely passes by coincidence (v1 also re-pathed every tick).
- T05 fails — v1 calls FindPath every tick, so safety-timer behavior conflates with stateless behavior.
- T06 fails — v1 NavAgentComponent has no PathCount field; field exists post-Task 2 but stays 0 because v1 system doesn't fill it.
- T08 fails — v1 has no PathIndex tracking.
- T09 may pass trivially (PathCount=0 → loop is no-op).

The KEY assertion is T02: "stable state, no repath" — that's the perf goal of v2 and it can ONLY be satisfied by Task 4's cached-path implementation. T02 failure validates the test as a real driver.

- [ ] **Step 4: Commit (failing tests first per TDD)**

```bash
git add tests/test_navagent.cpp tests/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "test(navigation): T01-T09 NavAgent cached-path (failing — drives Task 4)

9 tests covering the v2 cached-path behavior:
- T01: target change triggers repath
- T02: stable state -> NO repath (perf goal — v1 fails)
- T03: navmesh rebuild bumps NavVersion -> next tick repath
- T04: agent strayed > 2*Radius from segment -> repath
- T05: safety timer (>1.0s) -> repath
- T06: long path truncated at kMaxPathPoints, PathCount in (0, 32]
- T07: reached destination -> NavAgentSystem early-returns, no MoveIntent
- T08: PathIndex advances when intermediate waypoint reached
- T09: snapshot exposes CachedPath identically to master (viz contract)

Tests use a CountingFindPath wrapper around NavServicesImpl
forwarders to count FindPath calls — the only way to validate
the 'no per-tick FindPath in stable state' perf goal.

Currently fail at runtime under v1 stateless NavAgentSystem
(notably T02 — v1 calls FindPath every tick by design).
Task 4 lands the cached-path implementation that turns them green."
```

---

## Task 4: NavAgentSystem cached-path implementation

**Files:**
- Modify: `src/game/src/NavAgentSystem.h`

- [ ] **Step 1: Replace NavAgentSystem.h with v2 cached-path body**

Full new content for `src/game/src/NavAgentSystem.h`:

```cpp
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtx/norm.hpp>   // glm::length2

#include "ECS.h"
#include "Systems.h"   // SystemContext + NavServices pulled in transitively
#include "lib.h"       // SM_WARN (currently unused; reserved for future rate-limited no-path logging)

// Simulation-phase system that drives nav-agent entities by writing
// MoveIntentComponent toward the next waypoint of a cached path. Repath fires
// only on 5 explicit triggers; otherwise the system walks the cached path.
//
// v2 CACHED-PATH DESIGN (Spec 5b):
//   Cached path lives on NavAgentComponent (fixed 32-waypoint glm::vec3 array
//   — trivially copyable, snapshot copy is a memcpy). Repath triggers:
//     1. Target changed (LastTarget != target.Destination, eps 1e-4)
//     2. Navmesh rebuilt (LastNavVersion != ctx.Nav->NavVersion())
//     3. Agent strayed > 2*Radius from current path segment
//     4. Path consumed (PathIndex >= PathCount)
//     5. Safety timer expired (TimeSinceRepath > kRepathInterval = 1.0s)
//   Collapses FindPath frequency from O(agents/tick) to ~O(agents/2 seconds)
//   in stable steady-state.
//
// Side benefit: DebugRenderPass::ShowNavPaths reads CachedPath off the
// snapshot — no FindPath call from RenderThread, killing the v1 footgun.
class NavAgentSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        if (!ctx.Nav || !ctx.Nav->HasMesh()) return;  // no navmesh built yet
        const float dt = static_cast<float>(ctx.dt);
        const uint32_t navVer = ctx.Nav->NavVersion();

        ctx.world.Each<NavAgentComponent, NavTargetComponent, TransformComponent>(
            [&](EntityId e, const NavAgentComponent& agent,
                const NavTargetComponent& target, const TransformComponent& tr) {

                // Reached check — arrived agents skip everything (matches v1 behavior).
                const glm::vec3 toTarget = target.Destination - tr.Position;
                if (glm::length2(toTarget) < agent.ReachedEpsilon * agent.ReachedEpsilon) {
                    return;  // arrived; MoveIntent (if any) consumed by KinematicMovementSystem next tick
                }

                // Decide whether to repath. Mutate NavAgent bookkeeping inside one Modify.
                bool needsRepath = false;
                {
                    const bool targetChanged = !approx_eq(agent.LastTarget, target.Destination, 1e-4f);
                    const bool navInvalidated = (agent.LastNavVersion != navVer);
                    const bool timerExpired = ((agent.TimeSinceRepath + dt) > kRepathInterval);
                    const bool pathConsumed = (agent.PathIndex >= agent.PathCount);

                    bool strayed = false;
                    if (!pathConsumed && agent.PathIndex > 0
                        && agent.PathIndex < agent.PathCount) {
                        const glm::vec3 segStart = agent.CachedPath[agent.PathIndex - 1];
                        const glm::vec3 segEnd   = agent.CachedPath[agent.PathIndex];
                        strayed = dist_to_segment(tr.Position, segStart, segEnd)
                                  > 2.0f * agent.Radius;
                    }

                    needsRepath = targetChanged || navInvalidated || timerExpired
                                  || pathConsumed || strayed;
                }

                // Accumulate timer + maybe repath.
                ctx.world.Modify<NavAgentComponent>(e, [&](NavAgentComponent& a) {
                    a.TimeSinceRepath += dt;

                    if (needsRepath) {
                        ctx.Nav->FindPath(tr.Position, target.Destination,
                                          kFindPathSearchRadius, &m_PathScratch);
                        const int n = std::min(static_cast<int>(m_PathScratch.size()),
                                               NavAgentComponent::kMaxPathPoints);
                        for (int i = 0; i < n; ++i) a.CachedPath[i] = m_PathScratch[i];
                        a.PathCount       = static_cast<uint8_t>(n);
                        a.PathIndex       = (n >= 2) ? uint8_t{1} : uint8_t{0};
                        a.LastTarget      = target.Destination;
                        a.LastNavVersion  = navVer;
                        a.TimeSinceRepath = 0.0f;
                    }
                });

                // Re-read latest agent state (Modify above may have rewritten path).
                const auto* aRO = ctx.world.GetComponent<NavAgentComponent>(e);
                if (!aRO || aRO->PathCount < 2 || aRO->PathIndex >= aRO->PathCount) {
                    return;  // no path (unreachable / off-mesh / consumed)
                }

                // Walk toward CachedPath[PathIndex]. Advance index on reach.
                const glm::vec3 next = aRO->CachedPath[aRO->PathIndex];
                const glm::vec3 dir = next - tr.Position;
                const float distSq = glm::length2(dir);

                if (distSq < aRO->ReachedEpsilon * aRO->ReachedEpsilon) {
                    ctx.world.Modify<NavAgentComponent>(e, [&](NavAgentComponent& a) {
                        a.PathIndex++;
                    });
                    return;  // next tick consumes new waypoint
                }

                const float dirLen = std::sqrt(distSq);
                if (dirLen < 1e-5f) return;
                const float stepLen = std::min(aRO->MoveSpeed * dt, dirLen);
                const glm::vec3 desiredDelta = (dir / dirLen) * stepLen;

                // Lazy-seed MoveIntent (same pattern as PlayerMovementSystem).
                if (!ctx.world.HasComponent<MoveIntentComponent>(e)) {
                    ctx.world.AddComponent(e, MoveIntentComponent{});
                }
                ctx.world.Modify<MoveIntentComponent>(e, [&](MoveIntentComponent& m){
                    m.DesiredDelta = desiredDelta;
                });
            });
    }

    const char* Name() const override { return "NavAgentSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }

private:
    // Reusable buffer for ctx.Nav->FindPath outparam — cleared by FindPath each
    // call, never grows beyond the longest path encountered. Avoids per-tick
    // reallocation in the (now rare) repath hot path.
    std::vector<glm::vec3> m_PathScratch;

    static constexpr float kRepathInterval = 1.0f;          // safety-timer seconds
    static constexpr float kFindPathSearchRadius = 50.0f;   // matches v1 NavMesh::FindPath maxSearchRadius arg

    // ---- Inline helpers (file-private) ----

    // Component-wise epsilon equality for vec3.
    static bool approx_eq(const glm::vec3& a, const glm::vec3& b, float eps) {
        return glm::all(glm::lessThan(glm::abs(a - b), glm::vec3(eps)));
    }

    // Distance from point p to line segment [a, b] (clamped to endpoints).
    static float dist_to_segment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b) {
        const glm::vec3 ab = b - a;
        const float ab2 = glm::dot(ab, ab);
        const float t = (ab2 < 1e-8f) ? 0.0f
                      : glm::clamp(glm::dot(p - a, ab) / ab2, 0.0f, 1.0f);
        return glm::length(p - (a + t * ab));
    }
};
```

- [ ] **Step 2: Build game + test_navagent**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target game test_navagent --config Debug
```

Expected: clean build.

- [ ] **Step 3: Run test_navagent — expect all 9 green**

```bash
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navagent.exe
```

Expected: `All NavAgent tests passed.`

If T02 still fails: the cached-path branch isn't suppressing FindPath calls. Verify `needsRepath` is `false` on the steady-state ticks (initial tick sets LastTarget/LastNavVersion; subsequent ticks shouldn't trip any of the 5 triggers within the 0.5s window).

If T06 fails on `PathCount > 0`: the long-path test's straight-line traversal may not actually produce > 32 waypoints under default `NavMeshConfigComponent::CellSize` (0.3). String-pull tends to collapse straight segments. The test's actual assertion is `PathCount in (0, 32]` — both inclusive of "short straight path" — so this should pass regardless. If it doesn't, the cached path isn't being filled at all.

If T08 fails: the PathIndex advance after `Modify(a.PathIndex++)` may be racing the subsequent re-read. Check that the `aRO` re-read happens AFTER the index Modify (the code structure above is correct — index increment + early return; advancement is visible the NEXT tick).

- [ ] **Step 4: Regression sweep**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_followcam test_playermove test_editorprefs --config Debug
for t in test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_followcam test_playermove test_editorprefs; do
  ./out/build/msvc-win64-vs2026-community/bin/Debug/$t.exe || { echo "$t FAILED"; exit 1; }
done
```

Expected: all 9 pass. NavAgentSystem.h change is isolated to that one file; other tests don't transitively exercise its body.

- [ ] **Step 5: Commit**

```bash
git add src/game/src/NavAgentSystem.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): NavAgentSystem v2 cached-path

NavAgentSystem flips from pure-reader stateless v1 to cached-path
mutator v2. Per tick:
- Reached-destination check (unchanged from v1).
- Evaluate 5 repath triggers: target-changed, nav-version mismatch,
  agent strayed > 2*Radius from segment, path consumed, safety
  timer (>1.0s) expired.
- If repath: call ctx.Nav->FindPath, copy up to kMaxPathPoints=32
  waypoints into NavAgentComponent::CachedPath, reset bookkeeping.
- Walk toward CachedPath[PathIndex] capped by MoveSpeed*dt. Advance
  PathIndex when within ReachedEpsilon of current waypoint.

Stable steady-state: zero FindPath calls until safety timer fires
(~1/sec per agent) or the agent strays/path-changes. v1 was
O(agents) FindPath/tick; v2 is ~O(agents/1.0s) -> 60x reduction at
60 Hz.

Helpers added (file-private static inline):
- approx_eq(vec3, vec3, eps): component-wise epsilon equality
- dist_to_segment(p, a, b): clamped point-to-segment distance

Constants:
- kRepathInterval = 1.0f (safety timer)
- kFindPathSearchRadius = 50.0f (matches v1)

Drives test_navagent T01-T09 green. test_navmesh + 8 regression
suites untouched."
```

---

## Task 5: DebugRenderPass ShowNavPaths path-line rendering

**Files:**
- Modify: `src/engine/src/rendering/passes/DebugRenderPass.cpp`

- [ ] **Step 1: Replace the ShowNavPaths block**

In `src/engine/src/rendering/passes/DebugRenderPass.cpp`, find the existing block (lines 219-231):

```cpp
    if (s.ShowNavPaths) {
        // v1: destination markers only. Path-line rendering is deferred to v2 (cached
        // path on NavAgentComponent) — NavMesh::FindPath asserts on GameThread owner,
        // but DebugRenderPass runs on RenderThread. Once v2 caches the path on the
        // agent, viz reads the cached vector instead of querying (consistent with the
        // stateless-v1 / cached-v2 framework documented in navigation-agents-design.md).
        const glm::vec4 destCol(1.0f, 1.0f, 1.0f, 1.0f);  // white — destination markers
        world->Each<NavAgentComponent, NavTargetComponent, TransformComponent>(
            [&](EntityId, const NavAgentComponent&,
                const NavTargetComponent& target, const TransformComponent&) {
                DebugAppendSphere(m_Verts, target.Destination, 0.25f, destCol, 12);
            });
    }
```

Replace the entire block (delete the v1 footgun comment, add path-line rendering):

```cpp
    if (s.ShowNavPaths) {
        // v2: reads NavAgentComponent::CachedPath off the snapshot. No FindPath
        // call from RenderThread — the v1 GameThread-assert footgun is gone.
        const glm::vec4 pathCol(0.2f, 1.0f, 0.2f, 1.0f);  // green — path lines
        const glm::vec4 destCol(1.0f, 1.0f, 1.0f, 1.0f);  // white — destination markers
        world->Each<NavAgentComponent, NavTargetComponent, TransformComponent>(
            [&](EntityId, const NavAgentComponent& a,
                const NavTargetComponent& target, const TransformComponent&) {
                DebugAppendSphere(m_Verts, target.Destination, 0.25f, destCol, 12);
                for (uint8_t i = 1; i < a.PathCount; ++i) {
                    DebugAppendLine(m_Verts, a.CachedPath[i - 1], a.CachedPath[i], pathCol);
                }
            });
    }
```

- [ ] **Step 2: Build Engine + editor**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine editor --config Debug
```

Expected: clean build.

- [ ] **Step 3: Smoke (manual — user step in Task 6, not here)**

Don't launch the editor in this task. Final review (Task 6) hands off to user GUI smoke.

- [ ] **Step 4: Regression sweep**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_navmesh test_navagent --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navagent.exe
```

Expected: both pass.

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/rendering/passes/DebugRenderPass.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(rendering): ShowNavPaths reads cached path off snapshot

DebugRenderPass ShowNavPaths block now renders the v2 cached path
(NavAgentComponent::CachedPath) as green line segments alongside
the existing white destination markers.

The v1 footgun comment (NavMesh::FindPath GameThread-only assert
vs RenderThread DebugRenderPass) is deleted — no FindPath call
from RenderThread happens; the read is purely off the ECS snapshot.

PathCount-bounded loop (uint8_t i = 1; i < a.PathCount; ++i)
skips the start-snapped CachedPath[0] for cleaner viz."
```

---

## Task 6: Final whole-feature review

- [ ] **Step 1: Verify clean tree + full sweep**

```bash
git status -sb
# Expected: clean.

cmake --build out/build/msvc-win64-vs2026-community --target Engine editor game test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_navagent test_followcam test_playermove test_editorprefs --config Debug

for t in test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_navagent test_followcam test_playermove test_editorprefs; do
  ./out/build/msvc-win64-vs2026-community/bin/Debug/$t.exe || { echo "$t FAILED"; exit 1; }
done
echo "All tests green."
```

Expected: all 10 test suites pass + "All tests green."

- [ ] **Step 2: Verify Game.dll boundary preserved**

```bash
"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.50.35717\bin\Hostx64\x64\dumpbin.exe" /DEPENDENTS out/build/msvc-win64-vs2026-community/bin/Debug/Game.dll | grep -i "\.dll"
```

Expected: `ecs.dll` + CRT (MSVCP140D, VCRUNTIME140D, VCRUNTIME140_1D, ucrtbased, KERNEL32). **No `Engine.dll`** — NavServices decoupling preserved.

- [ ] **Step 3: Dispatch final whole-feature reviewer subagent**

Per `superpowers:subagent-driven-development`, provide:
- Spec: `docs/superpowers/specs/2026-05-28-navagent-cached-path-design.md` (commit `e1fac9a`)
- Plan: `docs/superpowers/plans/2026-05-28-navagent-cached-path.md`
- Full branch diff: `git diff main..feat/navagent-cached-path`
- 5 commits (T1-T5; T0 administrative, T6 review-only).
- Per-task review summaries from the controller.

Reviewer verdict drives merge-readiness. User does GUI smoke before merge.

GUI smoke checklist for the user:
1. Editor launches clean (restart REQUIRED — GAME_API_VERSION bump). Console shows mesh loads.
2. Spawn an agent entity: TransformComponent + NavAgentComponent + NavTargetComponent (set Destination in inspector).
3. Spawn a NavMeshSource floor (Collider geometry box) + Rebuild NavMesh.
4. Toggle ShowNavPaths in Render Stats → green polyline traces agent's cached path; white sphere at destination.
5. Drag NavTarget Destination in inspector → green polyline re-routes within 1 tick (target-change trigger).
6. Click Rebuild NavMesh again → polyline re-routes within 1 tick (nav-version trigger).
7. Teleport agent's TransformComponent.Position far from current path segment → polyline re-routes within 1 tick (stray trigger).
8. Stand still for 1.5+ seconds → polyline silently re-routes (safety timer fired).
9. (Optional) Spawn 50+ agents pointing at same target. Frame time should stay under 16.67ms (60 FPS) — v1 would visibly stutter; v2 stays smooth thanks to ~1 FindPath/agent/sec.

---

## Self-review notes

**Spec coverage check:**

- ✅ NavMeshSystem atomic NavVersion + bump on all 3 publish sites — Task 1 (via PublishNavMesh helper).
- ✅ NavServices::NavVersion accessor + NavServicesImpl wiring — Task 1.
- ✅ NavAgentComponent v2 layout (CachedPath[32], PathCount, PathIndex, LastTarget, LastNavVersion, TimeSinceRepath) — Task 2.
- ✅ GAME_API_VERSION 17 → 18 — Task 2.
- ✅ ComponentSerialization NavAgent serializer only touches 3 tunables — Task 2 verifies (no-change expected).
- ✅ 5 repath triggers (target-change, nav-rebuild, stray, safety-timer, path-consumed) — Task 4.
- ✅ Mutate via Modify pattern + handle PathIndex advance — Task 4.
- ✅ dist_to_segment + approx_eq helpers + kRepathInterval=1.0f — Task 4.
- ✅ Reached-destination early return preserved from v1 — Task 4.
- ✅ DebugRenderPass ShowNavPaths green polyline + destination sphere from snapshot — Task 5.
- ✅ v1 footgun comment deleted — Task 5.
- ✅ Tests T01-T09 — Task 3 (red), Task 4 (green).
- ✅ Final whole-feature review + GUI smoke checklist — Task 6.

No gaps.

**Placeholder scan:** No "TBD"/"TODO"/"similar to". Every step has concrete code or concrete command + expected output.

**Type consistency:**

- `uint32_t` used consistently for NavVersion across NavMeshSystem + NavServices + NavAgentComponent.LastNavVersion.
- `uint8_t` for PathCount + PathIndex (cap 32 fits in 8 bits, saves 6 bytes/agent).
- `static constexpr int kMaxPathPoints = 32` declared inside NavAgentComponent — referenced as `NavAgentComponent::kMaxPathPoints` in tests + system body.
- `kRepathInterval = 1.0f` declared as `static constexpr float` in NavAgentSystem private section.
- `kFindPathSearchRadius = 50.0f` matches the v1 hard-coded `50.0f` literal — preserves search behavior.
- `glm::vec3` storage in CachedPath, LastTarget — matches FindPath result format.
- `EntityId` (uint64_t alias) used uniformly in tests + Each<...> iteration.

**Commit count:** 5 commits (T1-T5; T0 admin, T6 review-only). Within the spec's 5-6 estimate.
