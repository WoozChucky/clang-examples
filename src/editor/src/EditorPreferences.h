#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "RenderStats.h" // CullingSettings, DebugDrawSettings, ShadowSettings (+ ENGINE_API getters)

// Editor-only persistence of the RenderStats panel toggles. Saved next to the
// executable as editor_preferences.json; the runtime never touches it.
namespace EditorPreferences {

constexpr auto     DEFAULT_PREFERENCES_PATH = "editor_preferences.json";
constexpr uint32_t PREFERENCES_VERSION      = 1;

// Pure: serialize the three RenderStats settings structs to the preferences JSON shape.
// No globals touched -> unit-testable without an Engine link.
inline nlohmann::json PrefsToJson(const CullingSettings& culling,
                                  const DebugDrawSettings& debug,
                                  const ShadowSettings& shadows) {
    return nlohmann::json{
        {"version", PREFERENCES_VERSION},
        {"culling", {
            {"frustum", culling.Enabled},
        }},
        {"debugDraw", {
            {"lightGizmos",   debug.ShowLightGizmos},
            {"cameraFrustum", debug.ShowCameraFrustum},
            {"selectedAABB",  debug.ShowSelectedAABB},
            {"wireframe",     debug.Wireframe},
            {"grid",          debug.ShowGrid},
        }},
        {"shadows", {
            {"enabled", shadows.Enabled},
            {"bias",    shadows.Bias},
        }},
    };
}

// Pure: apply present keys from `j` onto the out-params. Missing/wrong-typed keys leave
// the corresponding field unchanged (caller-supplied defaults survive a partial/old
// file). Never throws (checked contains/type tests, no `.at()`).
inline void PrefsFromJson(const nlohmann::json& j,
                          CullingSettings& culling,
                          DebugDrawSettings& debug,
                          ShadowSettings& shadows) {
    if (j.contains("culling") && j["culling"].is_object()) {
        const auto& c = j["culling"];
        if (c.contains("frustum") && c["frustum"].is_boolean()) culling.Enabled = c["frustum"].get<bool>();
    }
    if (j.contains("debugDraw") && j["debugDraw"].is_object()) {
        const auto& d = j["debugDraw"];
        if (d.contains("lightGizmos")   && d["lightGizmos"].is_boolean())   debug.ShowLightGizmos   = d["lightGizmos"].get<bool>();
        if (d.contains("cameraFrustum") && d["cameraFrustum"].is_boolean()) debug.ShowCameraFrustum = d["cameraFrustum"].get<bool>();
        if (d.contains("selectedAABB")  && d["selectedAABB"].is_boolean())  debug.ShowSelectedAABB  = d["selectedAABB"].get<bool>();
        if (d.contains("wireframe")     && d["wireframe"].is_boolean())     debug.Wireframe         = d["wireframe"].get<bool>();
        if (d.contains("grid")          && d["grid"].is_boolean())          debug.ShowGrid          = d["grid"].get<bool>();
    }
    if (j.contains("shadows") && j["shadows"].is_object()) {
        const auto& s = j["shadows"];
        if (s.contains("enabled") && s["enabled"].is_boolean()) shadows.Enabled = s["enabled"].get<bool>();
        if (s.contains("bias")    && s["bias"].is_number())     shadows.Bias    = s["bias"].get<float>();
    }
}

// Read editor_preferences.json at `path` and apply it onto the live RenderStats globals.
// Missing file -> true (defaults stay). Parse error -> false (WARN; globals untouched).
bool Load(const std::string& path);

// Serialize the live RenderStats globals to `path`. I/O failure -> false (WARN).
bool Save(const std::string& path);

} // namespace EditorPreferences
