#pragma once
#include <vector>
#include <cmath>
#include <glm/glm.hpp>

// One vertex of debug line geometry. Line-list topology (pairs of verts = segments).
struct DebugVertex {
    glm::vec3 Position;
    glm::vec4 Color;
};

inline void DebugAppendLine(std::vector<DebugVertex>& out, const glm::vec3& a, const glm::vec3& b,
                            const glm::vec4& color) {
    out.push_back({a, color});
    out.push_back({b, color});
}

// 12 edges of the AABB [mn, mx] (24 verts).
inline void DebugAppendBox(std::vector<DebugVertex>& out, const glm::vec3& mn, const glm::vec3& mx,
                           const glm::vec4& color) {
    const glm::vec3 c[8] = {
        {mn.x,mn.y,mn.z},{mx.x,mn.y,mn.z},{mx.x,mx.y,mn.z},{mn.x,mx.y,mn.z},
        {mn.x,mn.y,mx.z},{mx.x,mn.y,mx.z},{mx.x,mx.y,mx.z},{mn.x,mx.y,mx.z},
    };
    const int e[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    for (auto& ed : e) DebugAppendLine(out, c[ed[0]], c[ed[1]], color);
}

// 3 axis-aligned circles (XY, XZ, YZ), `segments` line-segments each (3*segments*2 verts).
inline void DebugAppendSphere(std::vector<DebugVertex>& out, const glm::vec3& center, float radius,
                              const glm::vec4& color, int segments = 24) {
    const float kTwoPi = 6.28318530718f;
    for (int axis = 0; axis < 3; ++axis) {
        for (int i = 0; i < segments; ++i) {
            const float t0 = kTwoPi * (float(i)     / float(segments));
            const float t1 = kTwoPi * (float(i + 1) / float(segments));
            const float c0 = std::cos(t0) * radius, s0 = std::sin(t0) * radius;
            const float c1 = std::cos(t1) * radius, s1 = std::sin(t1) * radius;
            glm::vec3 p0, p1;
            if (axis == 0)      { p0 = {c0, s0, 0.0f}; p1 = {c1, s1, 0.0f}; }
            else if (axis == 1) { p0 = {c0, 0.0f, s0}; p1 = {c1, 0.0f, s1}; }
            else                { p0 = {0.0f, c0, s0}; p1 = {0.0f, c1, s1}; }
            DebugAppendLine(out, center + p0, center + p1, color);
        }
    }
}

// Shaft from->to plus a 4-line arrowhead at `to`.
inline void DebugAppendArrow(std::vector<DebugVertex>& out, const glm::vec3& from, const glm::vec3& to,
                             const glm::vec4& color) {
    DebugAppendLine(out, from, to, color);
    glm::vec3 dir = to - from;
    const float len = glm::length(dir);
    if (len < 1e-5f) return;
    dir /= len;
    const glm::vec3 up = (std::abs(dir.y) < 0.99f) ? glm::vec3(0,1,0) : glm::vec3(1,0,0);
    const glm::vec3 right = glm::normalize(glm::cross(dir, up));
    const glm::vec3 u     = glm::normalize(glm::cross(right, dir));
    const float h = len * 0.2f;
    const glm::vec3 base = to - dir * h;
    DebugAppendLine(out, to, base + right * (h * 0.5f), color);
    DebugAppendLine(out, to, base - right * (h * 0.5f), color);
    DebugAppendLine(out, to, base + u     * (h * 0.5f), color);
    DebugAppendLine(out, to, base - u     * (h * 0.5f), color);
}

// 12 edges of the frustum = the unprojected NDC cube. z in {0,1} (GLM ZO depth).
inline void DebugAppendFrustum(std::vector<DebugVertex>& out, const glm::mat4& invViewProj,
                               const glm::vec4& color) {
    const glm::vec3 ndc[8] = {
        {-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0},
        {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1},
    };
    glm::vec3 corners[8];
    for (int i = 0; i < 8; ++i) {
        const glm::vec4 w = invViewProj * glm::vec4(ndc[i], 1.0f);
        corners[i] = glm::vec3(w) / w.w;
    }
    const int e[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    for (auto& ed : e) DebugAppendLine(out, corners[ed[0]], corners[ed[1]], color);
}
