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

// Capsule outline = cylinder body (4 vertical seams + 2 horizontal circles at the cylinder
// caps) + 2 sphere caps at the dome centers. center is the midpoint between the domes;
// halfHeight is half the cylinder length (so total tip-to-tip height = 2*(halfHeight+radius)).
// Cylinder runs along world Y. Wireframe-only -> the sphere/cylinder overlap is invisible.
inline void DebugAppendCapsule(std::vector<DebugVertex>& out, const glm::vec3& center,
                               float radius, float halfHeight,
                               const glm::vec4& color, int segments = 16) {
    if (radius < 1e-4f) radius = 1e-4f;
    if (halfHeight < 0.0f) halfHeight = 0.0f;
    if (segments < 4) segments = 4;
    const float kTwoPi = 6.28318530718f;
    const glm::vec3 topCenter    = center + glm::vec3(0.0f,  halfHeight, 0.0f);
    const glm::vec3 bottomCenter = center + glm::vec3(0.0f, -halfHeight, 0.0f);

    // 2 horizontal circles at the cylinder caps (XZ plane at top/bottom).
    for (int i = 0; i < segments; ++i) {
        const float t0 = kTwoPi * (float(i)     / float(segments));
        const float t1 = kTwoPi * (float(i + 1) / float(segments));
        const glm::vec3 a(std::cos(t0) * radius, 0.0f, std::sin(t0) * radius);
        const glm::vec3 b(std::cos(t1) * radius, 0.0f, std::sin(t1) * radius);
        DebugAppendLine(out, topCenter    + a, topCenter    + b, color);
        DebugAppendLine(out, bottomCenter + a, bottomCenter + b, color);
    }

    // 4 vertical seam lines connecting the cap circles (at +X, -X, +Z, -Z).
    const glm::vec3 seams[4] = {
        { radius, 0.0f, 0.0f }, { -radius, 0.0f, 0.0f },
        { 0.0f, 0.0f,  radius }, { 0.0f, 0.0f, -radius },
    };
    for (const glm::vec3& s : seams) {
        DebugAppendLine(out, bottomCenter + s, topCenter + s, color);
    }

    // Dome caps: 2 full spheres at the dome centers (wireframe overlap is invisible).
    DebugAppendSphere(out, topCenter,    radius, color, segments);
    DebugAppendSphere(out, bottomCenter, radius, color, segments);
}

// Wireframe cylinder (Y-axis aligned). `bottomCenter` = base center, height extends +Y.
// segments per ring. Two horizontal rings (XZ plane) + 4 vertical seams at +X/-X/+Z/-Z.
// (segments * 2 + 4) * 2 verts total. Matches the cylinder body of DebugAppendCapsule.
inline void DebugAppendCylinder(std::vector<DebugVertex>& out, const glm::vec3& bottomCenter,
                                float radius, float height,
                                const glm::vec4& color, int segments = 12) {
    if (radius < 1e-4f) radius = 1e-4f;
    if (height < 0.0f)  height = 0.0f;
    if (segments < 4)   segments = 4;
    const float kTwoPi = 6.28318530718f;
    const glm::vec3 topCenter = bottomCenter + glm::vec3(0.0f, height, 0.0f);

    // 2 horizontal circles (XZ) at base + top.
    for (int i = 0; i < segments; ++i) {
        const float t0 = kTwoPi * (float(i)     / float(segments));
        const float t1 = kTwoPi * (float(i + 1) / float(segments));
        const glm::vec3 a(std::cos(t0) * radius, 0.0f, std::sin(t0) * radius);
        const glm::vec3 b(std::cos(t1) * radius, 0.0f, std::sin(t1) * radius);
        DebugAppendLine(out, bottomCenter + a, bottomCenter + b, color);
        DebugAppendLine(out, topCenter    + a, topCenter    + b, color);
    }

    // 4 vertical seams.
    const glm::vec3 seams[4] = {
        { radius, 0.0f, 0.0f }, { -radius, 0.0f, 0.0f },
        { 0.0f, 0.0f,  radius }, { 0.0f, 0.0f, -radius },
    };
    for (const glm::vec3& s : seams) {
        DebugAppendLine(out, bottomCenter + s, topCenter + s, color);
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

// Camera-snapped ground grid on Y=0. The patch is centered on the camera's XZ snapped
// to `step`, so a finite grid follows the camera (feels infinite, never reaches the
// true horizon). Lines where world X==0 / Z==0 fall inside the patch use the axis
// colors; all others use gridColor. Line-list: pairs of verts = segments.
inline void DebugAppendGrid(std::vector<DebugVertex>& out,
                            const glm::vec3& cameraPos,
                            float halfExtent, float step,
                            const glm::vec4& gridColor,
                            const glm::vec4& axisXColor,
                            const glm::vec4& axisZColor) {
    if (step < 1e-3f) step = 1e-3f;
    if (halfExtent < step) halfExtent = step;
    // Snap the patch center to the grid so lines appear stationary as the camera moves.
    const float cx = std::round(cameraPos.x / step) * step;
    const float cz = std::round(cameraPos.z / step) * step;
    const int   n  = static_cast<int>(halfExtent / step); // lines each side of center
    const float axisEps = 0.5f * step;
    // Lines parallel to Z (vary X): span the full Z extent. World Z axis is at X==0.
    for (int i = -n; i <= n; ++i) {
        const float gx = cx + static_cast<float>(i) * step;
        const glm::vec4 col = (std::abs(gx) < axisEps) ? axisZColor : gridColor;
        DebugAppendLine(out, {gx, 0.0f, cz - halfExtent}, {gx, 0.0f, cz + halfExtent}, col);
    }
    // Lines parallel to X (vary Z): span the full X extent. World X axis is at Z==0.
    for (int i = -n; i <= n; ++i) {
        const float gz = cz + static_cast<float>(i) * step;
        const glm::vec4 col = (std::abs(gz) < axisEps) ? axisXColor : gridColor;
        DebugAppendLine(out, {cx - halfExtent, 0.0f, gz}, {cx + halfExtent, 0.0f, gz}, col);
    }
}
