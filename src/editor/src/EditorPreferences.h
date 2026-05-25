#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "RenderStats.h" // CullingSettings, DebugDrawSettings, ShadowSettings (+ ENGINE_API getters)
#include "EditorCameraState.h"

// Editor-only persistence of the RenderStats panel toggles + the editor camera pose.
// Saved next to the executable as editor_preferences.json; the runtime never touches it.
namespace EditorPreferences {

constexpr auto     DEFAULT_PREFERENCES_PATH = "editor_preferences.json";
constexpr uint32_t PREFERENCES_VERSION      = 1;

// Pure: serialize the three RenderStats settings structs to the preferences JSON shape.
// No globals touched -> unit-testable without an Engine link.
inline nlohmann::json PrefsToJson(const CullingSettings& culling,
                                  const DebugDrawSettings& debug,
                                  const ShadowSettings& shadows,
                                  const EditorCameraState& camera) {
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
        {"camera", {
            {"position", { camera.Position.x, camera.Position.y, camera.Position.z }},
            {"yaw",      camera.Yaw},
            {"pitch",    camera.Pitch},
            {"flySpeed", camera.FlySpeed},
        }},
    };
}

// Pure: apply present keys from `j` onto the out-params. Missing/wrong-typed keys leave
// the corresponding field unchanged (caller-supplied defaults survive a partial/old
// file). Never throws (checked contains/type tests, no `.at()`).
inline void PrefsFromJson(const nlohmann::json& j,
                          CullingSettings& culling,
                          DebugDrawSettings& debug,
                          ShadowSettings& shadows,
                          EditorCameraState& camera) {
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
    if (j.contains("camera") && j["camera"].is_object()) {
        const auto& cam = j["camera"];
        if (cam.contains("position") && cam["position"].is_array() && cam["position"].size() == 3) {
            const auto& p = cam["position"];
            if (p[0].is_number() && p[1].is_number() && p[2].is_number())
                camera.Position = glm::vec3(p[0].get<float>(), p[1].get<float>(), p[2].get<float>());
        }
        if (cam.contains("yaw")      && cam["yaw"].is_number())      camera.Yaw      = cam["yaw"].get<float>();
        if (cam.contains("pitch")    && cam["pitch"].is_number())    camera.Pitch    = cam["pitch"].get<float>();
        if (cam.contains("flySpeed") && cam["flySpeed"].is_number()) camera.FlySpeed = cam["flySpeed"].get<float>();
    }
}

// Read editor_preferences.json at `path`: applies toggles to the live RenderStats
// globals and fills `camera` from the file. Seed `camera` with the current pose so a
// missing "camera" block leaves it. Missing file -> true. Parse error -> false.
bool Load(const std::string& path, EditorCameraState& camera);

// Serialize the live RenderStats globals + the given camera to `path`. I/O failure -> false.
bool Save(const std::string& path, const EditorCameraState& camera);

} // namespace EditorPreferences
