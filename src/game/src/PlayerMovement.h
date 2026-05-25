#pragma once

#include <cmath> // std::cos, std::sin

#include <glm/glm.hpp>

#include "ECS.h" // InputStateComponent + KEY_* codes (via input.h)

// Planar move from WASD, rotated by `yawRadians` so it aligns to a yawed (e.g. isometric)
// camera: W = "into the screen" (away from the camera), S toward it, A/D = screen left/right.
// With yawRadians == 0 this is plain world-axis movement (W = -Z, S = +Z, A = -X, D = +X).
// Diagonal is normalized so it isn't faster than a cardinal move. Y is never touched.
inline glm::vec3 ComputePlanarMove(const InputStateComponent& in, float speed, float dt, float yawRadians) {
    glm::vec3 dir(0.0f);
    if (in.KeysDown[KEY_W]) dir.z -= 1.0f;
    if (in.KeysDown[KEY_S]) dir.z += 1.0f;
    if (in.KeysDown[KEY_A]) dir.x -= 1.0f;
    if (in.KeysDown[KEY_D]) dir.x += 1.0f;
    if (dir.x != 0.0f || dir.z != 0.0f) dir = glm::normalize(dir);
    // Rotate the input around +Y by the camera yaw (length-preserving) so the world-axis
    // WASD vector aligns to the camera's screen axes. yaw 0 leaves it as world-axis.
    const float c = std::cos(yawRadians), s = std::sin(yawRadians);
    const glm::vec3 aligned(dir.x * c + dir.z * s, 0.0f, -dir.x * s + dir.z * c);
    return aligned * (speed * dt);
}
