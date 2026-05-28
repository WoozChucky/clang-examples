#pragma once

#include <cmath> // std::sin, std::cos

#include <glm/glm.hpp> // glm::vec3, glm::radians, glm::clamp, glm::normalize

// Sun light direction for a FIXED sun angle, matching the engine convention used by
// DayNightSystem's animated path: the vector points FROM the sun toward the scene, so an
// overhead (noon) sun points straight down (y == -1) and a horizon sun has y == 0.
//
//   elevDeg    : 0 = on the horizon, 90 = directly overhead. Clamped to [0, 90].
//   azimuthDeg : compass rotation of the horizontal component around the +Y axis.
//
// Returns a unit vector.
inline glm::vec3 SunDirectionFromAngles(float elevDeg, float azimuthDeg)
{
    const float E = glm::radians(glm::clamp(elevDeg, 0.0f, 90.0f));
    const float A = glm::radians(azimuthDeg);
    return glm::normalize(glm::vec3(
        std::cos(E) * std::sin(A),
        -std::sin(E),
        std::cos(E) * std::cos(A)));
}
