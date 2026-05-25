#pragma once

#include <glm/glm.hpp>

#include "ECS.h" // InputStateComponent + KEY_* codes (via input.h)

// World-axis planar move from WASD. Forward = -Z, back = +Z, left = -X, right = +X.
// Diagonal is normalized so it isn't faster than a cardinal move. Y is never touched.
inline glm::vec3 ComputePlanarMove(const InputStateComponent& in, float speed, float dt) {
    glm::vec3 dir(0.0f);
    if (in.KeysDown[KEY_W]) dir.z -= 1.0f;
    if (in.KeysDown[KEY_S]) dir.z += 1.0f;
    if (in.KeysDown[KEY_A]) dir.x -= 1.0f;
    if (in.KeysDown[KEY_D]) dir.x += 1.0f;
    if (dir.x != 0.0f || dir.z != 0.0f) dir = glm::normalize(dir);
    return dir * (speed * dt);
}
