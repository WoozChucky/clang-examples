#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

// A resolved camera the render passes consume: world-space View/Projection + eye position.
// Trivially copyable so it can ride a Seqlock (single-writer lock-free publish).
struct CameraView {
    glm::mat4 View{1.0f};
    glm::mat4 Projection{1.0f};
    glm::vec3 Position{0.0f};
};
