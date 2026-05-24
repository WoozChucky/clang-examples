#pragma once

#include <glm/vec3.hpp>

#include "Engine.h"

// Editor-tunable fog parameters. Single instance lives in Fog.cpp and is
// exported from Engine.dll so the mesh pass (Engine.dll) and the editor panel
// (editor.exe) share ONE copy — same pattern as GetShadowSettings(). Touched
// only on the RenderThread (mesh pass + ImGui overlay run there).
struct FogSettings {
    bool      Enabled      = true;
    float     DayDensity   = 0.008f;                 // barely-there daytime haze
    float     NightDensity = 0.09f;                  // noticeable at night
    glm::vec3 DayColor     = glm::vec3(0.60f, 0.70f, 0.80f); // hazy blue-grey
    glm::vec3 NightColor   = glm::vec3(0.03f, 0.04f, 0.08f); // dark blue
};

ENGINE_API FogSettings& GetFogSettings();

// Resolved fog for one frame: the color used for BOTH the scene clear and the
// geometry blend, plus the exponential density.
struct FogFrame {
    glm::vec3 Color   = glm::vec3(0.0f);
    float     Density = 0.0f;
};

// Pure: maps the directional sun's elevation to fog color + density.
// elevation = clamp(-sunDir.y, 0, 1): 1 at noon, 0 at/below the horizon (night).
ENGINE_API FogFrame ComputeFog(const glm::vec3& sunDir, const FogSettings& s);
