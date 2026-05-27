#pragma once

#include <algorithm>
#include <vector>

#include <glm/glm.hpp>

#include "ECS.h"
#include "Systems.h"   // SystemContext + NavServices pulled in transitively
#include "lib.h"       // SM_WARN (currently unused; reserved for future rate-limited no-path logging)

// Simulation-phase system that drives nav-agent entities by writing
// MoveIntentComponent toward the next waypoint of a freshly-queried path.
//
// v1 STATELESS DESIGN:
//   Each tick: ctx.Nav->FindPath(transform, target) -> walk toward path[1]
//   capped at MoveSpeed * dt. No path cache. CPU cost = O(agents *
//   FindPath). Acceptable for ~10 agents on current scene scale.
//
// v2 UPGRADE PATH (when agent count > 20 or FindPath cost dominates):
//   Add cached path + PathIndex + TimeSinceRepath to NavAgentComponent.
//   Repath only on target-change OR PathIndex-stale OR time-elapsed.
//   See navigation-agents-design.md for migration notes.
//
// Engine-decoupled v2: NavMesh queries now go through ctx.Nav->FindPath
// (function-pointer table on SystemContext) instead of NavMesh::FindPath
// directly. m_PathScratch is a per-system reusable buffer so the FindPath
// outparam vector doesn't reallocate per call.
class NavAgentSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        if (!ctx.Nav || !ctx.Nav->HasMesh()) return;  // no navmesh built yet — nothing to path
        const float dt = static_cast<float>(ctx.dt);

        ctx.world.Each<NavAgentComponent, NavTargetComponent, TransformComponent>(
            [&](EntityId e, const NavAgentComponent& agent,
                const NavTargetComponent& target, const TransformComponent& tr) {

                // Reached check first — arrived agents skip FindPath entirely.
                const glm::vec3 toTarget = target.Destination - tr.Position;
                const float dist2 = glm::dot(toTarget, toTarget);
                if (dist2 < agent.ReachedEpsilon * agent.ReachedEpsilon) {
                    return;  // arrived; stop emitting intent (does NOT remove target)
                }

                // Stateless query — every tick. v2 caches this.
                ctx.Nav->FindPath(tr.Position, target.Destination, 50.0f, &m_PathScratch);
                if (m_PathScratch.size() < 2) {
                    return;  // no path (unreachable / off-mesh start/end / FindPath fail)
                }

                // Walk toward path[1] (path[0] is start-position-snapped-to-navmesh).
                const glm::vec3 nextWaypoint = m_PathScratch[1];
                const glm::vec3 dir = nextWaypoint - tr.Position;
                const float dirLen = glm::length(dir);
                if (dirLen < 1e-5f) return;  // degenerate (waypoint coincides with position)

                const glm::vec3 normalized = dir / dirLen;
                const float stepLen = std::min(agent.MoveSpeed * dt, dirLen);
                const glm::vec3 desiredDelta = normalized * stepLen;

                // Lazy-seed MoveIntent on first move (same pattern as PlayerMovementSystem
                // from movement-decoupling spec). KinematicMovementSystem (Physics phase)
                // consumes + clears.
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
    // Reusable buffer for ctx.Nav->FindPath outparam — cleared by FindPath
    // each call, never grows beyond the longest path encountered. Avoids
    // per-tick reallocation in the hot path.
    std::vector<glm::vec3> m_PathScratch;
};
