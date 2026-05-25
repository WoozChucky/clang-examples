#pragma once

#include <glm/vec3.hpp>

#include "ECS.h" // FogComponent

// Resolved fog for one frame: the color used for BOTH the scene clear and the
// geometry blend, plus the exponential density.
struct FogFrame {
    glm::vec3 Color   = glm::vec3(0.0f);
    float     Density = 0.0f;
};

// Pure: maps the directional sun's elevation to fog color + density.
// elevation = clamp(-sunDir.y, 0, 1): 1 at noon, 0 at/below the horizon (night).
FogFrame ComputeFog(const glm::vec3& sunDir, const FogComponent& fog);
