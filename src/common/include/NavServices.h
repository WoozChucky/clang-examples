#pragma once

#include <cstdint>
#include <vector>

#include <glm/vec3.hpp>

#include "ECS.h"   // EntityId — already in common/include, no engine dep

// Engine-provided navigation service table. Engine populates an instance at
// startup (NavServicesImpl::Init in src/engine/src/navigation/); GameThread
// threads a pointer through SystemContext each tick. Game systems
// (NavObstacleSync, NavAgentSystem) call through this table instead of
// NavMeshSystem::Instance() — keeps Game.dll's link graph free of Engine.dll.
//
// All function pointers are non-null after NavServicesImpl::Init. Callable
// from GameThread only (matches NavMeshSystem's GameThread-only contract).
//
// Field append-only for forward compatibility — reordering or removing fields
// breaks Game.dll binary compat (function pointer offsets shift).
struct NavServices {
    // ---- NavMesh existence + query ----

    // True if a navmesh has been built (current published shared_ptr non-null).
    // Cheap atomic_load on engine side.
    bool (*HasMesh)();

    // Fill outPath with string-pulled path positions. Empty on no path /
    // unreachable / off-mesh. Caller owns the vector (clear+populate semantics).
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
    uint32_t (*FindObstacleForEntity)(EntityId e);   // 0 if not tracked
    void     (*UntrackEntity)(EntityId e);

    // ---- Surface constraint (navmesh-constrained movement) ----

    // Constrain a move to the navmesh surface (wall-slide via dtNavMeshQuery::
    // moveAlongSurface). Off-mesh start → pulls toward nearest poly (recovery).
    // No mesh → returns desiredEnd unchanged. GameThread only.
    glm::vec3 (*MoveAlongSurface)(const glm::vec3& start, const glm::vec3& desiredEnd);
};
