#pragma once

#include <cmath> // std::cos, std::sin

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp> // glm::lookAtRH, glm::perspectiveRH_ZO

#include "CameraView.h" // CameraView (View/Projection/Position)

// Fixed-angle isometric follow-camera parameters (no runtime controls).
struct FollowCameraParams {
    float Distance;     // eye distance from the look-at target
    float ElevationDeg; // angle above the horizon (90 = straight down)
    float YawDeg;       // rotation around the target's Y axis
    float FovDeg;       // vertical field of view
    float TargetHeight; // look-at point raised above the player's origin
    float Near;
    float Far;
};

// PoE2-ish starting values. Dial via game-DLL hot-reload.
inline constexpr FollowCameraParams kPoE2Follow{
    /*Distance*/ 22.0f, /*ElevationDeg*/ 55.0f, /*YawDeg*/ 45.0f,
    /*FovDeg*/ 40.0f, /*TargetHeight*/ 1.0f, /*Near*/ 0.1f, /*Far*/ 1000.0f
};

// Build a fixed-angle follow camera looking at `targetPos` (+ TargetHeight on Y).
inline CameraView ComputeFollowCamera(const glm::vec3& targetPos,
                                      const FollowCameraParams& p,
                                      float aspect) {
    const glm::vec3 target = targetPos + glm::vec3(0.0f, p.TargetHeight, 0.0f);
    const float E = glm::radians(p.ElevationDeg);
    const float A = glm::radians(p.YawDeg);
    // Unit direction from the target up to the eye (|offsetDir| == 1).
    const glm::vec3 offsetDir(std::cos(E) * std::sin(A), std::sin(E), std::cos(E) * std::cos(A));
    const glm::vec3 eye = target + p.Distance * offsetDir;

    CameraView v;
    v.View       = glm::lookAtRH(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
    v.Projection = glm::perspectiveRH_ZO(glm::radians(p.FovDeg), aspect, p.Near, p.Far);
    v.Position   = eye;
    return v;
}
