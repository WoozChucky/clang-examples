#pragma once

#include <algorithm>

#include <glm/glm.hpp>

#include "ECS.h"
#include "Systems.h"
#include "lib.h"   // SM_WARN (currently unused; reserved for future rate-limited no-path logging)

#include "navigation/NavMeshSystem.h"
#include "navigation/NavMesh.h"

// Simulation-phase system that drives nav-agent entities by writing
// MoveIntentComponent toward the next waypoint of a freshly-queried path.
//
// v1 STATELESS DESIGN:
//   Each tick: FindPath(transform, target) -> walk toward path[1] capped at
//   MoveSpeed * dt. No path cache. CPU cost = O(agents * FindPath).
//   Acceptable for ~10 agents on current scene scale.
//
// v2 UPGRADE PATH (when agent count > 20 or FindPath cost dominates):
//   Add cached path + PathIndex + TimeSinceRepath to NavAgentComponent.
//   Repath only on target-change OR PathIndex-stale OR time-elapsed.
//   See navigation-agents-design.md for migration notes.
class NavAgentSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const auto nav = NavMeshSystem::Instance().Current();
        if (!nav) return;  // no navmesh built yet — nothing to path
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
                const auto path = nav->FindPath(tr.Position, target.Destination);
                if (path.size() < 2) {
                    return;  // no path (unreachable / off-mesh start/end / FindPath fail)
                }

                // Walk toward path[1] (path[0] is start-position-snapped-to-navmesh).
                const glm::vec3 nextWaypoint = path[1].Position;
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
};
