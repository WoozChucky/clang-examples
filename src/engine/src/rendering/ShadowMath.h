#pragma once
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// The sun casts (is above the horizon) when its travel direction points downward (y < 0).
// eps trims the near-horizontal degenerate sliver (infinite shadows at the exact horizon).
inline bool IsSunUp(const glm::vec3& sunDir, float eps = 0.02f) {
    return sunDir.y < -eps;
}

// Orthographic light view-projection framing the scene bounds (center, radius) along the sun
// direction. sunDir = the direction the light travels (away from the sun). RH + ZO depth ([0,1]).
//
// `radius` is the bounding-SPHERE radius of the scene (in real use 0.5*length(aabbMax-aabbMin),
// the AABB's circumradius, enclosing all 8 corners). A tight [-r, r] ortho box exactly contains
// the orthographic projection of that sphere (a disc of radius r fits a 2r square) for ANY light
// direction, giving maximal shadow-map resolution.
inline glm::mat4 ComputeLightViewProj(const glm::vec3& center, float radius,
                                      const glm::vec3& sunDir, float nearExtend = 0.0f) {
    const glm::vec3 d = glm::normalize(sunDir);
    const float r = (radius > 1e-3f) ? radius : 1.0f;
    const glm::vec3 up = (std::abs(d.y) > 0.99f) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    const float ext = (nearExtend > 0.0f) ? nearExtend : 0.0f;
    const glm::vec3 eye = center - d * (r * 2.0f + ext);
    const glm::mat4 view = glm::lookAtRH(eye, center, up);
    const glm::mat4 proj = glm::orthoRH_ZO(-r, r, -r, r, 0.0f, 4.0f * r + ext);
    return proj * view;
}

// Bounding sphere of a region (world space).
struct ShadowSphere { glm::vec3 center{0.0f}; float radius = 0.0f; };

// Bound the camera frustum slice [camera near, shadowDistance] with a sphere, in world space.
// Reconstructs the 8 view-space corners from the projection (eye at the view-space origin, so a
// corner's xy scales linearly with depth), caps the far corners at shadowDistance, transforms to
// world via inverse(view), then fits a sphere (center = mean, radius = max corner distance).
// Assumes a perspective projection (the game camera is perspective).
inline ShadowSphere FrustumSliceSphere(const glm::mat4& camView, const glm::mat4& camProj,
                                       float shadowDistance) {
    const glm::mat4 invP = glm::inverse(camProj);
    const glm::mat4 invV = glm::inverse(camView);
    const glm::vec2 ndc[4] = { {-1,-1}, {1,-1}, {1,1}, {-1,1} };

    glm::vec3 corners[8];
    for (int i = 0; i < 4; ++i) {
        glm::vec4 vn = invP * glm::vec4(ndc[i].x, ndc[i].y, 0.0f, 1.0f); // near plane (ZO z=0)
        vn /= vn.w;
        const float nearDepth = -vn.z;                 // view looks down -Z (RH)
        const float dist = glm::max(shadowDistance, nearDepth + 1e-3f);
        const float scale = dist / nearDepth;          // similar triangles from the eye (origin)
        const glm::vec4 vf(vn.x * scale, vn.y * scale, -dist, 1.0f);
        corners[i]     = glm::vec3(invV * vn);
        corners[i + 4] = glm::vec3(invV * vf);
    }

    glm::vec3 c(0.0f);
    for (const auto& p : corners) c += p;
    c /= 8.0f;
    float r2 = 0.0f;
    for (const auto& p : corners) r2 = glm::max(r2, glm::dot(p - c, p - c));
    return ShadowSphere{ c, std::sqrt(r2) };
}

// Quantize `center` to whole shadow-texel increments in the light's view plane so the shadow
// grid does not sub-pixel crawl as the camera pans. texelWorld = (2*radius) / shadowMapSize.
inline glm::vec3 SnapToTexelGrid(const glm::vec3& center, float radius, const glm::vec3& sunDir,
                                 uint32_t shadowMapSize) {
    const float r = (radius > 1e-3f) ? radius : 1.0f;
    const float texelWorld = (2.0f * r) / static_cast<float>(shadowMapSize);
    const glm::vec3 d = glm::normalize(sunDir);
    const glm::vec3 up = (std::abs(d.y) > 0.99f) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    const glm::vec3 right = glm::normalize(glm::cross(up, d));
    const glm::vec3 up2   = glm::cross(d, right);      // orthonormal light basis (right, up2, d)
    float cr = glm::dot(center, right);
    float cu = glm::dot(center, up2);
    const float cd = glm::dot(center, d);
    cr = std::round(cr / texelWorld) * texelWorld;     // snap the in-plane components
    cu = std::round(cu / texelWorld) * texelWorld;
    return right * cr + up2 * cu + d * cd;             // reconstruct (orthonormal basis)
}
