#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include <glm/glm.hpp>

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
                if (glm::dot(toTarget, toTarget) < agent.ReachedEpsilon * agent.ReachedEpsilon) {
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
                const float distSq = glm::dot(dir, dir);

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
