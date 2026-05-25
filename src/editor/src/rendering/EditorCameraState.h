#pragma once

#include <glm/vec3.hpp>

// Persistable editor fly-camera pose. Defaults mirror EditorCamera's defaults so a
// default-constructed state equals the camera's default pose.
struct EditorCameraState {
    glm::vec3 Position{0.0f, 5.0f, 10.0f};
    float     Yaw      = 0.0f;
    float     Pitch    = 0.0f;
    float     FlySpeed = 7.5f;
};
