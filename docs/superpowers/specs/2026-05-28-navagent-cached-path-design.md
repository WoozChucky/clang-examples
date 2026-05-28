# NavAgent v2 Cached Path — Design Spec

**Date:** 2026-05-28
**Status:** Approved
**Type:** Performance / scalability upgrade — closes the v2 upgrade path documented in `2026-05-27-navigation-agents-design.md`

---

## Goal

Eliminate per-tick `FindPath` per agent. Cache the path on `NavAgentComponent` (fixed-size array, trivially copyable). Repath only on explicit triggers. Collapses CPU cost from `O(agents × FindPath)` per tick to `~O(agents)` per tick for the cheap reached / waypoint-walk path, plus `~O(agents / kRepathInterval)` for repaths.

Side benefit: RenderThread `ShowNavPaths` viz can render the cached path off the snapshot — the v1 footgun (`NavMesh::FindPath` GameThread-only assert tripped by DebugRenderPass) is structurally gone.

---

## Architecture

- `NavAgentComponent` gains state fields: fixed-size path array, PathCount/PathIndex, LastTarget, LastNavVersion, TimeSinceRepath.
- `NavServices` gains `uint32_t (*NavVersion)()` accessor.
- `NavMeshSystem` gains `std::atomic<uint32_t> m_NavVersion`, incremented at end of every `Rebuild`.
- `NavAgentSystem` switches from pure-reader to mutator: per tick, check 5 repath conditions (target-change, nav-version mismatch, safety-timer expiry, path-consumed, agent strayed); repath if any fire; otherwise walk toward current waypoint and advance PathIndex on reach.
- `DebugRenderPass` `ShowNavPaths` reads `CachedPath` off the snapshot — no FindPath call from RenderThread.

No new components, no new files (test target excepted).

---

## NavAgentComponent layout

```cpp
struct NavAgentComponent {
    // v1 user-authored tunables — unchanged
    float MoveSpeed      = 3.0f;
    float Radius         = 0.5f;
    float ReachedEpsilon = 0.10f;

    // v2 cached-path state — runtime, NOT serialized
    static constexpr int kMaxPathPoints = 32;
    glm::vec3 CachedPath[kMaxPathPoints]{};
    uint8_t   PathCount       = 0;     // valid waypoints in CachedPath
    uint8_t   PathIndex       = 0;     // next waypoint agent walks toward
    glm::vec3 LastTarget{0.0f};        // change-detection vs target.Destination
    uint32_t  LastNavVersion  = 0;     // navmesh-rebuild invalidation
    float     TimeSinceRepath = 0.0f;  // safety-timer accumulator
};
```

**Size:** ~420 bytes/agent (glm::vec3 is 12 bytes unaligned in this codebase; uint8_t pair gets 2-byte pad before next vec3). 100 agents × 60 Hz snapshot publish = ~2.5 MB/s — acceptable at current scene scale.

**Trivially copyable preserved** — snapshot copy stays a memcpy through `ComponentArray::CopyFrom`. No heap allocation per snapshot per agent.

**Serialization (`ComponentSerialization.h`):** `to_json`/`from_json` keep saving ONLY the 3 v1 tunables. Runtime state (CachedPath, PathIndex, LastTarget, LastNavVersion, TimeSinceRepath) defaults on load. `from_json` already handles missing fields gracefully via `if (j.contains(...))`.

---

## Repath logic

Per tick, per agent (after the existing reached-destination early-out):

```cpp
const float dt = static_cast<float>(ctx.dt);

ctx.world.Modify<NavAgentComponent>(e, [&](NavAgentComponent& a) {
    a.TimeSinceRepath += dt;

    const uint32_t navVer = ctx.Nav->NavVersion();
    const bool targetChanged  = !approx_eq(a.LastTarget, target.Destination, 1e-4f);
    const bool navInvalidated = (a.LastNavVersion != navVer);
    const bool timerExpired   = (a.TimeSinceRepath > kRepathInterval);  // 1.0f
    const bool pathConsumed   = (a.PathIndex >= a.PathCount);

    bool strayed = false;
    if (!pathConsumed && a.PathIndex > 0) {
        const glm::vec3 segStart = a.CachedPath[a.PathIndex - 1];
        const glm::vec3 segEnd   = a.CachedPath[a.PathIndex];
        strayed = dist_to_segment(tr.Position, segStart, segEnd) > 2.0f * a.Radius;
    }

    if (targetChanged || navInvalidated || timerExpired || pathConsumed || strayed) {
        ctx.Nav->FindPath(tr.Position, target.Destination, 50.0f, &m_PathScratch);
        const int n = std::min(static_cast<int>(m_PathScratch.size()),
                               NavAgentComponent::kMaxPathPoints);
        std::copy_n(m_PathScratch.data(), n, a.CachedPath);
        a.PathCount       = static_cast<uint8_t>(n);
        a.PathIndex       = (n >= 2) ? 1 : 0;   // skip start-snapped [0]
        a.LastTarget      = target.Destination;
        a.LastNavVersion  = navVer;
        a.TimeSinceRepath = 0.0f;
    }
});

// After Modify — re-read agent + check we have a path
const auto& aRO = *ctx.world.GetComponent<NavAgentComponent>(e);
if (aRO.PathCount < 2 || aRO.PathIndex >= aRO.PathCount) return;

// Walk toward CachedPath[PathIndex]
const glm::vec3 next = aRO.CachedPath[aRO.PathIndex];
const glm::vec3 dir  = next - tr.Position;
const float distSq   = glm::length2(dir);

if (distSq < aRO.ReachedEpsilon * aRO.ReachedEpsilon) {
    // Waypoint reached — advance PathIndex (next tick consumes new waypoint)
    ctx.world.Modify<NavAgentComponent>(e, [&](NavAgentComponent& a) {
        a.PathIndex++;
    });
    return;
}

const float dirLen = std::sqrt(distSq);
const float stepLen = std::min(aRO.MoveSpeed * dt, dirLen);
const glm::vec3 desiredDelta = (dir / dirLen) * stepLen;

// Lazy-seed MoveIntent (same pattern as v1)
if (!ctx.world.HasComponent<MoveIntentComponent>(e))
    ctx.world.AddComponent(e, MoveIntentComponent{});
ctx.world.Modify<MoveIntentComponent>(e, [&](MoveIntentComponent& m){
    m.DesiredDelta = desiredDelta;
});
```

**Constants** (in `NavAgentSystem.h`):
- `kRepathInterval = 1.0f` seconds (safety timer)
- Stray threshold = `2.0f * agent.Radius` (hard-coded — knockback systems can re-tune later)

**Helper:**

```cpp
// Squared-distance from point to line segment (clamped to endpoints).
static inline float dist_to_segment(const glm::vec3& p,
                                    const glm::vec3& a,
                                    const glm::vec3& b) {
    const glm::vec3 ab = b - a;
    const float t = glm::clamp(glm::dot(p - a, ab) / glm::max(glm::dot(ab, ab), 1e-8f),
                               0.0f, 1.0f);
    return glm::length(p - (a + t * ab));
}
```

Lives in NavAgentSystem.h as a `static inline` free function (header-private).

`approx_eq` for vec3: `glm::all(glm::lessThan(glm::abs(a - b), glm::vec3(eps)))`.

---

## NavServices addition

```cpp
struct NavServices {
    bool (*HasMesh)();
    void (*FindPath)(const glm::vec3& start, const glm::vec3& end,
                     float maxSearchRadius, std::vector<glm::vec3>* outPath);
    uint32_t (*NavVersion)();   // NEW: monotonic; bumped at end of every NavMeshSystem::Rebuild

    uint32_t (*AddCylinderObstacle)(const glm::vec3& pos, float radius, float height);
    uint32_t (*AddBoxObstacle)(const glm::vec3& bmin, const glm::vec3& bmax);
    void     (*RemoveObstacle)(uint32_t handle);

    void     (*TrackObstacleForEntity)(EntityId e, uint32_t handle);
    uint32_t (*FindObstacleForEntity)(EntityId e);
    void     (*UntrackEntity)(EntityId e);
};
```

`NavVersion` inserted AFTER `FindPath`, BEFORE obstacle methods. Mid-struct insertion shifts obstacle-method offsets — `GAME_API_VERSION` bump required anyway (NavAgentComponent layout changes too).

**Engine side:**

```cpp
// NavMeshSystem.h
private:
    std::atomic<uint32_t> m_NavVersion{0};
public:
    uint32_t GetNavVersion() const { return m_NavVersion.load(std::memory_order_relaxed); }

// NavMeshSystem.cpp — end of Rebuild()
m_NavVersion.fetch_add(1, std::memory_order_relaxed);

// NavServicesImpl.cpp — Init()
services.NavVersion = []() { return NavMeshSystem::Instance().GetNavVersion(); };
```

Initial version `0`. Agent default `LastNavVersion = 0`. First-ever Rebuild bumps to `1` → all agents see `0 != 1` → repath. Subsequent rebuilds bump → same. Correct.

---

## DebugRenderPass `ShowNavPaths`

```cpp
if (s.ShowNavPaths) {
    const glm::vec4 pathCol(0.2f, 1.0f, 0.2f, 1.0f);  // green — path lines
    const glm::vec4 destCol(1.0f, 1.0f, 1.0f, 1.0f);  // white — destination markers
    world->Each<NavAgentComponent, NavTargetComponent, TransformComponent>(
        [&](EntityId, const NavAgentComponent& a,
            const NavTargetComponent& t, const TransformComponent&) {
            DebugAppendSphere(m_Verts, t.Destination, 0.25f, destCol, 12);
            for (uint8_t i = 1; i < a.PathCount; ++i) {
                DebugAppendLine(m_Verts, a.CachedPath[i-1], a.CachedPath[i], pathCol);
            }
        });
}
```

Replaces the existing destination-spheres-only block (DebugRenderPass.cpp:219-231). No FindPath call from RenderThread → the v1 footgun documented in the inline comment block dies; comment block deleted.

---

## File changes

**Modified:**
- `src/common/include/ECS.h` — NavAgentComponent layout (add 6 fields, keep 3).
- `src/common/include/NavServices.h` — append `NavVersion` function pointer.
- `src/common/include/Game.h` — bump `GAME_API_VERSION 17 → 18`.
- `src/common/include/ComponentSerialization.h` — verify NavAgentComponent (de)serializer still only touches v1 tunables (defensive; no behavior change expected).
- `src/engine/src/navigation/NavMeshSystem.h` — `m_NavVersion` atomic + `GetNavVersion()` accessor.
- `src/engine/src/navigation/NavMeshSystem.cpp` — bump in Rebuild.
- `src/engine/src/navigation/NavServicesImpl.cpp` — wire `services.NavVersion`.
- `src/game/src/NavAgentSystem.h` — replace stateless body with cached-path logic; add `dist_to_segment` + `approx_eq` helpers; add `kRepathInterval` constant.
- `src/engine/src/rendering/passes/DebugRenderPass.cpp` — replace ShowNavPaths block with path-line + destination-sphere.

**New:**
- `tests/test_navagent.cpp` (+ CMake target).

**No new components, no other file touches.**

---

## Testing

`tests/test_navagent.cpp` — direct NavAgentSystem::Update invocation with controlled ECS world + stubbed NavServices (test-local function-pointer table calling NavMeshSystem::Instance() pass-through OR fully stubbed for isolation).

Tests:

- **T01 — Target change triggers repath.** Build a navmesh (existing test infra: SpawnNavBox floor). Spawn agent at (0,0,0) with NavTarget(5,0,0). Tick once → path cached. Change target to (5,0,5). Tick once → LastTarget mismatch → FindPath called → CachedPath updated.
- **T02 — Stable state: no repath.** After T01-style initial cache, hold target steady, no nav rebuild, agent walks normally. Tick 30× (0.5s @ 60Hz). FindPath call count = 1 (initial only). PathCount unchanged across ticks.
- **T03 — Nav rebuild invalidates path.** Cache path. Call `NavMeshSystem::Instance().Rebuild(...)`. Tick once → LastNavVersion mismatch → FindPath called.
- **T04 — Strayed agent repaths.** Cache path. Teleport agent (mutate Transform.Position) to a point > 2*Radius away from current segment. Tick once → stray triggers repath.
- **T05 — Safety timer fires.** Hold target steady, no rebuild, agent stationary at far waypoint. Tick 70× at dt=1/60 → TimeSinceRepath > 1.0s → repath.
- **T06 — Path truncation + reflow.** Force FindPath to return > 32 waypoints (long path). Verify PathCount = 32. Walk agent past final waypoint → PathIndex >= PathCount → next-tick repath continues from new position.
- **T07 — Reached destination: NavAgentSystem early-returns.** Agent within ReachedEpsilon of `target.Destination`. Tick once → NavAgentSystem returns before any repath/walk logic. Any pre-existing MoveIntent is NOT touched by NavAgentSystem (matches v1 behavior — KinematicMovementSystem consumes-and-clears MoveIntent next tick). Assert: agent's `LastTarget`/`LastNavVersion`/`TimeSinceRepath`/`CachedPath` all unchanged across the tick.
- **T08 — PathIndex advances on waypoint reach.** Walk agent toward CachedPath[1]; once within ReachedEpsilon, next tick increments PathIndex to 2.
- **T09 — Snapshot exposes CachedPath to viz.** Mutate agent, publish snapshot, read back CachedPath via snapshot ECS pointer → memory equal to the master's CachedPath. Pins the "RenderThread reads off snapshot" contract.

`FindPath` call-count assertion in T02 uses a counter wrapping `ctx.Nav` (test-local NavServices instance). Verifies the perf goal (no per-tick FindPath in stable state).

Regression: `test_followcam`, `test_playermove`, `test_navmesh` all green after change (test_navmesh doesn't exercise NavAgent — should be untouched). Editor-side GUI smoke covers visual repath behavior.

---

## Risks

1. **kMaxPathPoints = 32 cap.** Typical NavMesh string-pulled paths are 4–16 waypoints. Comfortable buffer at current scene scale. Mitigation: if profiling shows truncation common, bump cap (single-line constant change, no API impact).
2. **GAME_API_VERSION bump 17 → 18.** Editor restart required after this lands. Game.dll + editor.exe + runtime.exe all rebuild. Documented in CLAUDE.md hot-reload section already.
3. **Snapshot size growth.** ~420 bytes/agent vs v1's 12 bytes (~35× per-agent snapshot memory). At 100 agents: ~40 KB/snapshot. ECS snapshot pool reuses buffers, so amortized alloc cost is zero — only the memcpy grows. Negligible.
4. **Stray threshold hard-coded to 2*Radius.** Knockback systems pushing agents farther will trigger constant repaths. Mitigation: ship hard-coded; if knockback gameplay appears, expose as `NavAgentComponent::StrayMultiplier` field (additive, no breaking change).
5. **LastTarget float-epsilon compare (1e-4f = 0.1mm).** Game code jittering NavTarget by sub-epsilon per tick won't trigger repath. Acceptable — game shouldn't be jittering at that scale, and 1.0s safety timer catches genuine missed updates.
6. **NavVersion wraparound.** uint32_t wraps at ~4B rebuilds. At 1 rebuild/sec = 136 years. Non-issue.
7. **PathIndex starts at 1 to skip start-snapped [0].** Standard Recast string-pull behavior — CachedPath[0] is the start-position snapped to the navmesh (usually same as agent position). Walking toward it = degenerate zero-length step. Skipping is correct + matches v1 behavior at `m_PathScratch[1]`.
8. **MoveIntent NOT cleared on `pathConsumed && !targetReached`.** Edge case: agent hits the kMaxPathPoints cap, walks to CachedPath[31], increments PathIndex to 32, next tick triggers repath. Between those two ticks, a stale MoveIntent could persist — but KinematicMovementSystem consumes-and-clears MoveIntent each tick, so the lag is bounded to 1 tick (16ms @ 60Hz). Acceptable.

---

## Out-of-scope (explicit)

- **Entity-targeting** (`NavTargetComponent::TrackedEntity` field). Deferred to a future spec — orthogonal to path caching, keeps blast radius small.
- **Local avoidance / agent-agent collision.** Recast offers detour-crowd; not in scope. Current behavior preserved (no agent-agent awareness).
- **Dynamic path-recompute on obstacle add/remove.** NavObstacleComponent triggers a navmesh rebuild → NavVersion bump → agents repath. No per-obstacle event channel needed.
- **Path smoothing / cornering.** CachedPath is the raw Recast string-pull output. Smooth-corner walking deferred to a future visual-polish spec.
- **Vector-of-waypoints (heap path).** Considered, rejected: snapshot-per-tick heap alloc defeats v2 perf goal. Fixed array is the right shape at current scale.
- **Cross-tick async FindPath.** Considered, rejected: FindPath on the test scene is sub-millisecond per agent — not worth the async-state machinery. Revisit if profile shows otherwise.

---

## Commit estimate

5–6 commits:
1. `feat(navigation)`: NavMeshSystem nav-version counter + NavServices accessor.
2. `feat(ecs)`: NavAgentComponent v2 layout + GAME_API_VERSION bump.
3. `test(navigation)`: T01-T09 (TDD red — fail to compile until task 4).
4. `refactor(navigation)`: NavAgentSystem cached-path logic + helpers.
5. `feat(rendering)`: DebugRenderPass ShowNavPaths path-line rendering.
6. (Optional polish / fix-ups from review.)
