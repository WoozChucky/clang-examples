#pragma once

#include <glm/vec3.hpp>

#include "Engine.h"

// Editor-tunable procedural-sky parameters. Single instance in Sky.cpp, exported
// from Engine.dll (same pattern as GetFogSettings). Touched only on the RenderThread.
struct SkySettings {
    bool      Enabled       = true;
    glm::vec3 DayZenith     = glm::vec3(0.20f, 0.40f, 0.85f);
    glm::vec3 DayHorizon    = glm::vec3(0.70f, 0.80f, 0.95f);
    glm::vec3 NightZenith   = glm::vec3(0.01f, 0.02f, 0.06f);
    glm::vec3 NightHorizon  = glm::vec3(0.04f, 0.05f, 0.12f);
    glm::vec3 SunColor      = glm::vec3(1.00f, 0.95f, 0.80f);
    float     SunRadiusDeg  = 3.0f;
    float     SunGlow       = 64.0f;   // halo falloff exponent (higher = tighter)
    glm::vec3 MoonColor     = glm::vec3(0.80f, 0.85f, 1.00f);
    float     MoonRadiusDeg = 2.5f;
    float     MoonGlow      = 128.0f;
};

ENGINE_API SkySettings& GetSkySettings();
