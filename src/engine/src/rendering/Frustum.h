#pragma once
#include <cmath>
#include <glm/glm.hpp>

// Six frustum planes, each (n.x, n.y, n.z, d) with an inward-pointing unit normal:
// a point p is inside the half-space iff dot(n, p) + d >= 0.
struct Frustum { glm::vec4 Planes[6]; };

// Gribb-Hartmann extraction from a GLM (column-major) view-projection where clip = VP * v.
// rowI(m) = vec4(m[0][I], m[1][I], m[2][I], m[3][I]) (0-based). Depth-[0,1] (ZO) variant:
//   left   = row3 + row0     right = row3 - row0
//   bottom = row3 + row1     top   = row3 - row1
//   near   = row2            far   = row3 - row2
// near = row2 is the ZO-specific bit (the OpenGL [-1,1] form is row3 + row2).
inline Frustum ExtractFrustum(const glm::mat4& m)
{
    const glm::vec4 row0(m[0][0], m[1][0], m[2][0], m[3][0]);
    const glm::vec4 row1(m[0][1], m[1][1], m[2][1], m[3][1]);
    const glm::vec4 row2(m[0][2], m[1][2], m[2][2], m[3][2]);
    const glm::vec4 row3(m[0][3], m[1][3], m[2][3], m[3][3]);

    Frustum f;
    f.Planes[0] = row3 + row0; // left
    f.Planes[1] = row3 - row0; // right
    f.Planes[2] = row3 + row1; // bottom
    f.Planes[3] = row3 - row1; // top
    f.Planes[4] = row2;        // near
    f.Planes[5] = row3 - row2; // far

    for (glm::vec4& p : f.Planes)
    {
        const float len = glm::length(glm::vec3(p));
        if (len > 0.0f)
            p /= len;
    }
    return f;
}

// Transform a local-space AABB by m into a (looser) world-space AABB. Center+extents
// method: world center = m * center; world extent_i = sum_j |R[i][j]| * localExtent_j,
// where R is the upper-left 3x3 (R[i][j] math = a[j][i] in column-major GLM). No 8-corner loop.
inline void TransformAABB(const glm::mat4& m, glm::vec3 localMin, glm::vec3 localMax,
                          glm::vec3& outMin, glm::vec3& outMax)
{
    const glm::vec3 center  = 0.5f * (localMin + localMax);
    const glm::vec3 extents = 0.5f * (localMax - localMin);
    const glm::vec3 wCenter = glm::vec3(m * glm::vec4(center, 1.0f));
    const glm::mat3 a(m);
    glm::vec3 wExtents;
    for (int k = 0; k < 3; ++k)
        wExtents[k] = std::abs(a[0][k]) * extents.x
                    + std::abs(a[1][k]) * extents.y
                    + std::abs(a[2][k]) * extents.z;
    outMin = wCenter - wExtents;
    outMax = wCenter + wExtents;
}

// p-vertex test: the AABB is fully outside iff its positive vertex (the corner farthest
// along a plane's normal) is behind that plane. Returns true if possibly visible.
inline bool IsAABBVisible(const Frustum& f, glm::vec3 worldMin, glm::vec3 worldMax)
{
    for (const glm::vec4& plane : f.Planes)
    {
        const glm::vec3 n(plane);
        const glm::vec3 p(
            n.x >= 0.0f ? worldMax.x : worldMin.x,
            n.y >= 0.0f ? worldMax.y : worldMin.y,
            n.z >= 0.0f ? worldMax.z : worldMin.z);
        if (glm::dot(n, p) + plane.w < 0.0f)
            return false;
    }
    return true;
}
