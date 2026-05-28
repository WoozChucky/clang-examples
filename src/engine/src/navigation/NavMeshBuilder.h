#pragma once

#include <cstdint>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>

#include "Engine.h"

class ECS;

// Output of triangle collection: vertex/index/area arrays ready to feed into Recast.
// Verts are interleaved x/y/z floats; tris are 3 indices per triangle into verts;
// areas is one uint8 per triangle (Recast area ID).
struct ENGINE_API NavMeshTriangleSoup {
    std::vector<float>    Verts;   // 3 floats per vertex
    std::vector<int>      Tris;    // 3 ints per triangle (index into Verts/3)
    std::vector<uint8_t>  Areas;   // 1 uint8 per triangle
    glm::vec3             AabbMin{0.0f};
    glm::vec3             AabbMax{0.0f};
    bool                  Empty = true;
};

namespace NavMeshBuilder {

    // Walk all entities with NavMeshSourceComponent, resolve geometry per the Geometry
    // enum, transform to world space, concatenate. Logs SM_WARN per skipped entity.
    // Mesh-source entities pull CPU data from NavMeshSystem's mesh cache (populated
    // by GameThread when MeshUpload responses arrive); cache miss → SM_WARN + skip.
    ENGINE_API NavMeshTriangleSoup CollectTriangles(const ECS& world);

    // Append a unit box (12 tris) sized by halfExtents, transformed by worldXform, with areaId.
    ENGINE_API void TriangulateBox(const glm::vec3& halfExtents,
                                   const glm::mat4& worldXform,
                                   uint8_t areaId,
                                   NavMeshTriangleSoup& out);

    // Append a UV-sphere (segments per ring). radius in local space.
    // For nav purposes 8 segments is enough (Recast voxelizes anyway).
    ENGINE_API void TriangulateSphere(float radius,
                                      const glm::mat4& worldXform,
                                      uint8_t areaId,
                                      int segments,
                                      NavMeshTriangleSoup& out);

    // Append a capsule (cylinder + two hemispheres). radius + halfHeight in local space.
    ENGINE_API void TriangulateCapsule(float radius,
                                       float halfHeight,
                                       const glm::mat4& worldXform,
                                       uint8_t areaId,
                                       int segments,
                                       NavMeshTriangleSoup& out);

} // namespace NavMeshBuilder
