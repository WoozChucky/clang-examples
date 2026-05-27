#pragma once

#include <unordered_map>
#include <unordered_set>

#include <glm/glm.hpp>

#include "ECS.h"
#include "Systems.h"   // SystemContext + NavServices pulled in transitively
#include "lib.h"       // SM_WARN

// Physics-phase system that keeps dtTileCache obstacles in sync with the ECS
// NavObstacleComponent set. Per-tick diff:
//   - Component appears (no cached handle)   -> AddObstacle + Track + cache state
//   - Component disappears (entity vanished) -> RemoveObstacle + Untrack + erase cache
//   - Position moved > kPositionEpsilon      -> RemoveObstacle + AddObstacle (rebind)
//   - Shape or Size changed                  -> same rebind
//
// dtTileCache::update is NOT called here — GameThread::Run calls
// NavMeshSystem::Instance().Tick(dt) once per tick after all systems run, so
// other sites (Spec 3 agents) can request a tick without going through this
// system.
//
// Engine-decoupled v2: all NavMeshSystem calls now go through
// ctx.Nav->X() (function-pointer table on SystemContext) instead of
// NavMeshSystem::Instance().X(). Keeps Game.dll's link graph free of
// Engine.dll.
class NavObstacleSyncSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        if (!ctx.Nav || !ctx.Nav->HasMesh()) return;  // no navmesh built yet — nothing to carve

        m_VisitedThisTick.clear();

        ctx.world.Each<NavObstacleComponent, TransformComponent>(
            [&](EntityId e, const NavObstacleComponent& obs, const TransformComponent& tr) {
                m_VisitedThisTick.insert(e);
                const glm::vec3 worldPos = tr.Position + obs.Offset;

                auto& prev = m_EntityCache[e];   // default-constructs to zero state
                const uint32_t existing = ctx.Nav->FindObstacleForEntity(e);

                const bool needsAdd     = (existing == 0);
                const bool moved        = glm::distance(prev.Position, worldPos) > kPositionEpsilon;
                const bool shapeChanged = prev.Shape != obs.Shape || prev.Size != obs.Size;
                const bool needsRebind  = !needsAdd && (moved || shapeChanged);

                if (needsRebind) {
                    ctx.Nav->RemoveObstacle(existing);
                    ctx.Nav->UntrackEntity(e);
                }
                if (needsAdd || needsRebind) {
                    uint32_t h = 0;
                    if (obs.Shape == NavObstacleShape::Cylinder) {
                        h = ctx.Nav->AddCylinderObstacle(worldPos, obs.Size.x, obs.Size.y);
                    } else {  // Box
                        h = ctx.Nav->AddBoxObstacle(worldPos - obs.Size, worldPos + obs.Size);
                    }
                    if (h != 0) {
                        ctx.Nav->TrackObstacleForEntity(e, h);
                        prev = { worldPos, obs.Shape, obs.Size };
                    } else {
                        SM_WARN("NavObstacleSync: AddObstacle failed for entity %llu "
                                "(MaxObstacles cap reached?)", e);
                    }
                }
            });

        // GC entities that disappeared this tick (component removed or entity destroyed).
        for (auto it = m_EntityCache.begin(); it != m_EntityCache.end(); ) {
            if (!m_VisitedThisTick.contains(it->first)) {
                if (uint32_t h = ctx.Nav->FindObstacleForEntity(it->first); h != 0) {
                    ctx.Nav->RemoveObstacle(h);
                    ctx.Nav->UntrackEntity(it->first);
                }
                it = m_EntityCache.erase(it);
            } else {
                ++it;
            }
        }
    }

    const char* Name() const override { return "NavObstacleSyncSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Physics; }

private:
    struct CachedState {
        glm::vec3        Position{0.0f};
        NavObstacleShape Shape{NavObstacleShape::Cylinder};
        glm::vec3        Size{0.0f};
    };
    std::unordered_map<EntityId, CachedState> m_EntityCache;
    std::unordered_set<EntityId>              m_VisitedThisTick;
    static constexpr float kPositionEpsilon = 0.05f;  // 5 cm — below default cell size 0.3m
};
