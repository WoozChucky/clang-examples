#pragma once

#include <cstdint>
#include <cmath>
#include <glm/glm.hpp>

// Fixed 16-sample cosine/quadratic-weighted hemisphere kernel for SSAO. Lives in TANGENT space
// (+Z = surface normal); the shader rotates it into world space per pixel. Computed once on the
// CPU and uploaded to the AO shader CB.
struct SsaoKernel {
    static constexpr int Count = 16;
    glm::vec3 Samples[Count];
};

// Deterministic kernel (no RNG -> reproducible + unit-testable): golden-angle spiral over the
// +Z hemisphere, lengths quadratically pulled toward the origin so more samples cluster near the
// surface (standard SSAO weighting).
inline SsaoKernel MakeHemisphereKernel() {
    SsaoKernel k;
    const float golden = 2.39996323f; // golden angle (radians)
    for (int i = 0; i < SsaoKernel::Count; ++i) {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(SsaoKernel::Count);
        const float z = t;                       // +Z hemisphere
        const float r = std::sqrt(glm::max(0.0f, 1.0f - z * z));
        const float phi = static_cast<float>(i) * golden;
        glm::vec3 dir(r * std::cos(phi), r * std::sin(phi), z);
        float scale = static_cast<float>(i) / static_cast<float>(SsaoKernel::Count);
        scale = glm::mix(0.1f, 1.0f, scale * scale); // quadratic bias toward the surface
        k.Samples[i] = dir * scale;
    }
    return k;
}

// True when the stored surface is IN FRONT of the sample (closer to camera) by more than `bias`.
// View-space Z is negative (RH, looking down -Z), so closer = larger Z.
inline bool IsOccluded(float sampleViewZ, float occluderViewZ, float bias) {
    return occluderViewZ > (sampleViewZ + bias);
}

// Range falloff: 1 at the shaded point, linearly to 0 at `radius` and beyond (prevents haloing).
inline float RangeWeight(float dist, float radius) {
    if (radius <= 1e-5f) return 0.0f;
    return glm::clamp(1.0f - dist / radius, 0.0f, 1.0f);
}
