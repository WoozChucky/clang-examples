#include "Fog.h"

#include <glm/common.hpp> // glm::clamp, glm::mix

FogFrame ComputeFog(const glm::vec3& sunDir, const FogComponent& s)
{
    const float elevation = glm::clamp(-sunDir.y, 0.0f, 1.0f);
    FogFrame f;
    f.Density = glm::mix(s.NightDensity, s.DayDensity, elevation);
    f.Color   = glm::mix(s.NightColor,   s.DayColor,   elevation);
    return f;
}
