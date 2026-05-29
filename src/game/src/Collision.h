#pragma once

#include <algorithm>

#include <glm/glm.hpp>

#include "ECS.h"

struct CollisionAabb {
    glm::vec3 Min{0.0f};
    glm::vec3 Max{0.0f};
};

struct KinematicMoveResult {
    glm::vec3 AppliedDelta{0.0f};
    bool BlockedX = false;
    bool BlockedY = false;
    bool BlockedZ = false;
};

inline bool CollisionLayersMatch(const ColliderComponent& a, const ColliderComponent& b)
{
    return (a.Mask & b.Layer) != 0u && (b.Mask & a.Layer) != 0u;
}

inline glm::vec3 ComputeColliderHalfExtents(const TransformComponent& transform,
                                            const ColliderComponent& collider)
{
    const glm::vec3 scale = glm::abs(transform.Scale);
    switch (collider.Shape) {
        case ColliderShape::Sphere: {
            const float radius = std::max({
                collider.Size.x * scale.x,
                collider.Size.x * scale.y,
                collider.Size.x * scale.z,
                0.0f
            });
            return glm::vec3(radius);
        }
        case ColliderShape::Capsule: {
            const float radius = std::max({
                collider.Size.x * scale.x,
                collider.Size.x * scale.z,
                0.0f
            });
            const float halfHeight = std::max(collider.Size.y * scale.y, 0.0f);
            return glm::vec3(radius, halfHeight + radius, radius);
        }
        case ColliderShape::Box:
        default:
            return glm::max(glm::abs(collider.Size) * scale, glm::vec3(0.0001f));
    }
}

inline glm::vec3 ComputeColliderCenter(const TransformComponent& transform,
                                       const ColliderComponent& collider,
                                       const glm::vec3& position)
{
    return position + collider.Offset * transform.Scale;
}

// Distance from the transform origin DOWN to the collider's base, so that
// Position.Y = surfaceY + GroundOffset(...) places the collider base on the
// navmesh surface. Used by navmesh ground-snap. (collider base
// = center.y - halfExtents.y = Position.y + Offset.y*scale.y - halfExtents.y,
// so the origin sits halfExtents.y - Offset.y*scale.y above the base.)
inline float GroundOffset(const TransformComponent& transform, const ColliderComponent& collider)
{
    return ComputeColliderHalfExtents(transform, collider).y
         - collider.Offset.y * transform.Scale.y;
}

inline CollisionAabb BuildCollisionAabb(const TransformComponent& transform,
                                        const ColliderComponent& collider,
                                        const glm::vec3& position)
{
    const glm::vec3 center = ComputeColliderCenter(transform, collider, position);
    const glm::vec3 half = ComputeColliderHalfExtents(transform, collider);
    return CollisionAabb{ center - half, center + half };
}

inline bool AabbOverlaps(const CollisionAabb& a, const CollisionAabb& b)
{
    return a.Min.x < b.Max.x && a.Max.x > b.Min.x &&
           a.Min.y < b.Max.y && a.Max.y > b.Min.y &&
           a.Min.z < b.Max.z && a.Max.z > b.Min.z;
}

inline float AabbPenetrationSum(const CollisionAabb& a, const CollisionAabb& b)
{
    const float dx = std::min(a.Max.x, b.Max.x) - std::max(a.Min.x, b.Min.x);
    const float dy = std::min(a.Max.y, b.Max.y) - std::max(a.Min.y, b.Min.y);
    const float dz = std::min(a.Max.z, b.Max.z) - std::max(a.Min.z, b.Min.z);
    if (dx <= 0.0f || dy <= 0.0f || dz <= 0.0f)
        return 0.0f;
    return dx + dy + dz;
}

inline bool ColliderBlocksKinematicMove(const ColliderComponent& mover,
                                        EntityId moverEntity,
                                        const ColliderComponent& other,
                                        EntityId otherEntity)
{
    if (otherEntity == moverEntity)
        return false;
    if (mover.IsTrigger || other.IsTrigger)
        return false;
    if (!other.IsStatic)
        return false;
    return CollisionLayersMatch(mover, other);
}

inline bool CandidateMoveBlocked(const ECS& world,
                                 EntityId moverEntity,
                                 const TransformComponent& moverTransform,
                                 const ColliderComponent& moverCollider,
                                 const glm::vec3& fromPosition,
                                 const glm::vec3& candidatePosition)
{
    const CollisionAabb current = BuildCollisionAabb(moverTransform, moverCollider, fromPosition);
    const CollisionAabb candidate = BuildCollisionAabb(moverTransform, moverCollider, candidatePosition);

    bool blocked = false;
    world.Each<TransformComponent, ColliderComponent>([&](EntityId otherEntity,
                                                          const TransformComponent& otherTransform,
                                                          const ColliderComponent& otherCollider) {
        if (blocked || !ColliderBlocksKinematicMove(moverCollider, moverEntity, otherCollider, otherEntity))
            return;

        const CollisionAabb other = BuildCollisionAabb(otherTransform, otherCollider, otherTransform.Position);
        if (!AabbOverlaps(candidate, other))
            return;

        const bool overlappingAlready = AabbOverlaps(current, other);
        if (!overlappingAlready) {
            blocked = true;
            return;
        }

        const float currentPenetration = AabbPenetrationSum(current, other);
        const float candidatePenetration = AabbPenetrationSum(candidate, other);
        if (candidatePenetration > currentPenetration + 0.0001f)
            blocked = true;
    });
    return blocked;
}

inline KinematicMoveResult ResolveKinematicMove(const ECS& world,
                                                EntityId moverEntity,
                                                const TransformComponent& moverTransform,
                                                const ColliderComponent& moverCollider,
                                                const glm::vec3& desiredDelta)
{
    KinematicMoveResult result{};
    if (moverCollider.IsTrigger)
    {
        result.AppliedDelta = desiredDelta;
        return result;
    }

    glm::vec3 position = moverTransform.Position;
    auto tryAxis = [&](int axis, bool& blockedFlag) {
        const float delta = desiredDelta[axis];
        if (delta == 0.0f)
            return;

        glm::vec3 candidate = position;
        candidate[axis] += delta;
        if (CandidateMoveBlocked(world, moverEntity, moverTransform, moverCollider, position, candidate)) {
            blockedFlag = true;
            return;
        }

        position = candidate;
        result.AppliedDelta[axis] = delta;
    };

    tryAxis(0, result.BlockedX);
    tryAxis(1, result.BlockedY);
    tryAxis(2, result.BlockedZ);
    return result;
}

