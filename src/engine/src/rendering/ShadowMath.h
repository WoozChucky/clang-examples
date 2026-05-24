#pragma once
#include <cmath>
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
inline glm::mat4 ComputeLightViewProj(const glm::vec3& center, float radius, const glm::vec3& sunDir) {
    const glm::vec3 d = glm::normalize(sunDir);
    const float r = (radius > 1e-3f) ? radius : 1.0f;
    const glm::vec3 up = (std::abs(d.y) > 0.99f) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    const glm::vec3 eye = center - d * (r * 2.0f);
    const glm::mat4 view = glm::lookAtRH(eye, center, up);
    const glm::mat4 proj = glm::orthoRH_ZO(-r, r, -r, r, 0.0f, 4.0f * r);
    return proj * view;
}
