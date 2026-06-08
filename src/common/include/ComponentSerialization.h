#pragma once

// Single home for the json (de)serialization of ECS component types, shared by
// WorldManager (save/load) and unit tests. All free functions are `inline` so the
// header can be included in multiple translation units without ODR violations.

#include <algorithm>

#include <nlohmann/json.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "ECS.h"

#include "GlmJson.h"   // glm vec3/vec4 JSON helpers (extracted; ECS-free)
#include "AssetKey.h"
#include "AssetRegistry.h"
#include "lib.h"        // SM_WARN

inline void to_json(nlohmann::json& j, const TransformComponent& t) {
    j = nlohmann::json{{"Position", t.Position}, {"Rotation", t.Rotation}, {"Scale", t.Scale}};
}
inline void from_json(const nlohmann::json& j, TransformComponent& t) {
    j.at("Position").get_to(t.Position);
    j.at("Rotation").get_to(t.Rotation);
    j.at("Scale").get_to(t.Scale);
}

inline void to_json(nlohmann::json& j, const MeshComponent& t) {
    std::string key;
    if (const AssetRegistry* r = GetAssetRegistry(); r && r->MeshKeyForHandle) key = r->MeshKeyForHandle(t.MeshId);
    if (key.empty() && t.MeshId != kMissingAssetHandle)
        SM_WARN("MeshComponent: unresolved mesh handle %llu — saved with empty key", (unsigned long long)t.MeshId);
    j = nlohmann::json{{"MeshKey", key}, {"Visible", t.Visible}};
}
inline void from_json(const nlohmann::json& j, MeshComponent& t) {
    std::string key; j.at("MeshKey").get_to(key);
    t.MeshId = AssetKeyHash(key);   // "" -> kMissingAssetHandle
    j.at("Visible").get_to(t.Visible);
}

inline void to_json(nlohmann::json& j, const MaterialComponent& t) {
    std::string key;
    if (const AssetRegistry* r = GetAssetRegistry(); r && r->MaterialKeyForHandle) key = r->MaterialKeyForHandle(t.MaterialId);
    if (key.empty() && t.MaterialId != kMissingAssetHandle)
        SM_WARN("MaterialComponent: unresolved material handle %llu — saved with empty key", (unsigned long long)t.MaterialId);
    j = nlohmann::json{{"MaterialKey", key}, {"BaseColor", t.BaseColor}, {"Flags", t.Flags}};
}
inline void from_json(const nlohmann::json& j, MaterialComponent& t) {
    std::string key; j.at("MaterialKey").get_to(key);
    t.MaterialId = AssetKeyHash(key);
    j.at("BaseColor").get_to(t.BaseColor);
    j.at("Flags").get_to(t.Flags);
}

inline void to_json(nlohmann::json& j, const LightningComponent& t) {
    j = nlohmann::json{{"Type", t.Type}, {"Direction", t.Direction}, {"Color", t.Color}, {"Intensity", t.Intensity}, {"Range", t.Range}};
}
inline void from_json(const nlohmann::json& j, LightningComponent& t) {
    j.at("Type").get_to(t.Type);
    j.at("Direction").get_to(t.Direction);
    j.at("Color").get_to(t.Color);
    j.at("Intensity").get_to(t.Intensity);
    j.at("Range").get_to(t.Range);
}

inline void to_json(nlohmann::json& j, const TextComponent& t) {
    j = nlohmann::json{{"Text", t.Text}, {"Color", t.Color}, {"FontSize", t.FontSize}};
}
inline void from_json(const nlohmann::json& j, TextComponent& t) {
    j.at("Text").get_to(t.Text);
    j.at("Color").get_to(t.Color);
    j.at("FontSize").get_to(t.FontSize);
}

inline void to_json(nlohmann::json& j, const SunMarker&) { j = nlohmann::json::object(); }
inline void from_json(const nlohmann::json&, SunMarker&) {}

inline void to_json(nlohmann::json& j, const NavConstrainedComponent&) { j = nlohmann::json::object(); }
inline void from_json(const nlohmann::json&, NavConstrainedComponent&) {}

inline void to_json(nlohmann::json& j, const NavClassComponent& t) {
    j = nlohmann::json{ {"ClassId", t.ClassId} };
}
inline void from_json(const nlohmann::json& j, NavClassComponent& t) {
    if (j.contains("ClassId")) t.ClassId = static_cast<uint8_t>(j.at("ClassId").get<int>());
}

inline void to_json(nlohmann::json& j, const UIRectComponent& t) {
    j = nlohmann::json{
        {"Size", {{"X", t.Size.x}, {"Y", t.Size.y}}},
        {"Color", t.Color}   // glm::vec4 round-trips via its registered nlohmann serializer
    };
}
inline void from_json(const nlohmann::json& j, UIRectComponent& t) {
    t.Size.x = j.at("Size").at("X").get<float>();
    t.Size.y = j.at("Size").at("Y").get<float>();
    j.at("Color").get_to(t.Color);
}

inline void to_json(nlohmann::json& j, const NameComponent& t) {
    j = nlohmann::json{ {"Name", t.Name} };
}
inline void from_json(const nlohmann::json& j, NameComponent& t) {
    j.at("Name").get_to(t.Name);
}

inline void to_json(nlohmann::json& j, const SkeletonComponent& t) {
    j = nlohmann::json{ {"SkeletonId", t.SkeletonId} };
}
inline void from_json(const nlohmann::json& j, SkeletonComponent& t) {
    j.at("SkeletonId").get_to(t.SkeletonId);
}

inline void to_json(nlohmann::json& j, const AnimationComponent& t) {
    j = nlohmann::json{ {"ClipId", t.ClipId}, {"Time", t.Time}, {"Speed", t.Speed}, {"Looping", t.Looping}, {"Playing", t.Playing} };
}
inline void from_json(const nlohmann::json& j, AnimationComponent& t) {
    j.at("ClipId").get_to(t.ClipId);
    j.at("Time").get_to(t.Time);
    j.at("Speed").get_to(t.Speed);
    j.at("Looping").get_to(t.Looping);
    j.at("Playing").get_to(t.Playing);
}

inline void to_json(nlohmann::json& j, const StateScopeComponent& t) {
    j = nlohmann::json{{"StateMask", t.StateMask}};
}
inline void from_json(const nlohmann::json& j, StateScopeComponent& t) {
    j.at("StateMask").get_to(t.StateMask);
}


inline void to_json(nlohmann::json& j, const ColliderComponent& t) {
    j = nlohmann::json{
        // Shape goes through its underlying int so the JSON format stays the same
        // after switching to enum class (any existing world.json still loads).
        {"Shape", static_cast<uint8_t>(t.Shape)},
        {"Size", t.Size},
        {"Offset", t.Offset},
        {"IsTrigger", t.IsTrigger},
        {"IsStatic", t.IsStatic},
        {"Layer", t.Layer},
        {"Mask", t.Mask}
    };
}
inline void from_json(const nlohmann::json& j, ColliderComponent& t) {
    t.Shape = static_cast<ColliderShape>(j.at("Shape").get<uint8_t>());
    j.at("Size").get_to(t.Size);
    if (j.contains("Offset"))    j.at("Offset").get_to(t.Offset);
    if (j.contains("IsTrigger")) j.at("IsTrigger").get_to(t.IsTrigger);
    if (j.contains("IsStatic"))  j.at("IsStatic").get_to(t.IsStatic);
    if (j.contains("Layer"))     j.at("Layer").get_to(t.Layer);
    if (j.contains("Mask"))      j.at("Mask").get_to(t.Mask);
}

inline void to_json(nlohmann::json& j, const NavMeshSourceComponent& t) {
    j = nlohmann::json{
        {"AreaId",   t.AreaId},
        // Geometry as uint8_t for JSON stability (matches ColliderShape pattern).
        {"Geometry", static_cast<uint8_t>(t.Geometry)}
    };
}
inline void from_json(const nlohmann::json& j, NavMeshSourceComponent& t) {
    if (j.contains("AreaId"))   j.at("AreaId").get_to(t.AreaId);
    if (j.contains("Geometry")) t.Geometry = static_cast<NavMeshGeometrySource>(j.at("Geometry").get<uint8_t>());
}

inline void to_json(nlohmann::json& j, const NavObstacleComponent& t) {
    j = nlohmann::json{
        // Shape as uint8_t for JSON stability (matches ColliderShape / NavMeshGeometrySource pattern).
        {"Shape",  static_cast<uint8_t>(t.Shape)},
        {"Size",   t.Size},
        {"Offset", t.Offset}
    };
}
inline void from_json(const nlohmann::json& j, NavObstacleComponent& t) {
    if (j.contains("Shape"))  t.Shape = static_cast<NavObstacleShape>(j.at("Shape").get<uint8_t>());
    if (j.contains("Size"))   j.at("Size").get_to(t.Size);
    if (j.contains("Offset")) j.at("Offset").get_to(t.Offset);
}

inline void to_json(nlohmann::json& j, const NavAgentComponent& t) {
    j = nlohmann::json{
        {"MoveSpeed",      t.MoveSpeed},
        {"Radius",         t.Radius},
        {"ReachedEpsilon", t.ReachedEpsilon}
    };
}
inline void from_json(const nlohmann::json& j, NavAgentComponent& t) {
    if (j.contains("MoveSpeed"))      j.at("MoveSpeed").get_to(t.MoveSpeed);
    if (j.contains("Radius"))         j.at("Radius").get_to(t.Radius);
    if (j.contains("ReachedEpsilon")) j.at("ReachedEpsilon").get_to(t.ReachedEpsilon);
}

inline void to_json(nlohmann::json& j, const NavTargetComponent& t) {
    j = nlohmann::json{
        {"Destination", t.Destination}
    };
}
inline void from_json(const nlohmann::json& j, NavTargetComponent& t) {
    if (j.contains("Destination")) j.at("Destination").get_to(t.Destination);
}

// ----- Atmosphere components (new) -----

inline void to_json(nlohmann::json& j, const FogComponent& t) {
    j = nlohmann::json{
        {"Enabled", t.Enabled},
        {"DayDensity", t.DayDensity},
        {"NightDensity", t.NightDensity},
        {"DayColor", t.DayColor},
        {"NightColor", t.NightColor}};
}
inline void from_json(const nlohmann::json& j, FogComponent& t) {
    j.at("Enabled").get_to(t.Enabled);
    j.at("DayDensity").get_to(t.DayDensity);
    j.at("NightDensity").get_to(t.NightDensity);
    j.at("DayColor").get_to(t.DayColor);
    j.at("NightColor").get_to(t.NightColor);
}

inline void to_json(nlohmann::json& j, const SkyComponent& t) {
    j = nlohmann::json{
        {"Enabled", t.Enabled},
        {"DayZenith", t.DayZenith},
        {"DayHorizon", t.DayHorizon},
        {"NightZenith", t.NightZenith},
        {"NightHorizon", t.NightHorizon},
        {"SunColor", t.SunColor},
        {"SunRadiusDeg", t.SunRadiusDeg},
        {"SunGlow", t.SunGlow},
        {"MoonColor", t.MoonColor},
        {"MoonRadiusDeg", t.MoonRadiusDeg},
        {"MoonGlow", t.MoonGlow}};
}
inline void from_json(const nlohmann::json& j, SkyComponent& t) {
    j.at("Enabled").get_to(t.Enabled);
    j.at("DayZenith").get_to(t.DayZenith);
    j.at("DayHorizon").get_to(t.DayHorizon);
    j.at("NightZenith").get_to(t.NightZenith);
    j.at("NightHorizon").get_to(t.NightHorizon);
    j.at("SunColor").get_to(t.SunColor);
    j.at("SunRadiusDeg").get_to(t.SunRadiusDeg);
    j.at("SunGlow").get_to(t.SunGlow);
    j.at("MoonColor").get_to(t.MoonColor);
    j.at("MoonRadiusDeg").get_to(t.MoonRadiusDeg);
    j.at("MoonGlow").get_to(t.MoonGlow);
}

inline void to_json(nlohmann::json& j, const DayNightConfigComponent& t) {
    j = nlohmann::json{
        {"CycleSeconds", t.CycleSeconds},
        {"DayBrightness", t.DayBrightness},
        {"MoonIntensity", t.MoonIntensity},
        {"TwilightWidth", t.TwilightWidth},
        {"DayAmbient", t.DayAmbient},
        {"MoonColor", t.MoonColor},
        {"Mode", static_cast<int>(t.Mode)},
        {"StaticSunElevDeg", t.StaticSunElevDeg},
        {"StaticSunAzimuthDeg", t.StaticSunAzimuthDeg},
        {"ShowSunDisc", t.ShowSunDisc}};
}
inline void from_json(const nlohmann::json& j, DayNightConfigComponent& t) {
    j.at("CycleSeconds").get_to(t.CycleSeconds);
    j.at("DayBrightness").get_to(t.DayBrightness);
    j.at("MoonIntensity").get_to(t.MoonIntensity);
    j.at("TwilightWidth").get_to(t.TwilightWidth);
    j.at("DayAmbient").get_to(t.DayAmbient);
    j.at("MoonColor").get_to(t.MoonColor);
    // New fields are optional so existing world.json files still load (default = DynamicCycle).
    if (j.contains("Mode"))                t.Mode = static_cast<SkyMode>(j.at("Mode").get<int>());
    if (j.contains("StaticSunElevDeg"))    j.at("StaticSunElevDeg").get_to(t.StaticSunElevDeg);
    if (j.contains("StaticSunAzimuthDeg")) j.at("StaticSunAzimuthDeg").get_to(t.StaticSunAzimuthDeg);
    if (j.contains("ShowSunDisc"))         j.at("ShowSunDisc").get_to(t.ShowSunDisc);
}

inline void to_json(nlohmann::json& j, const NavClassConfig& c) {
    j = nlohmann::json{
        {"AgentRadius",   c.AgentRadius},
        {"AgentHeight",   c.AgentHeight},
        {"AgentMaxClimb", c.AgentMaxClimb}
    };
}
inline void from_json(const nlohmann::json& j, NavClassConfig& c) {
    if (j.contains("AgentRadius"))   j.at("AgentRadius").get_to(c.AgentRadius);
    if (j.contains("AgentHeight"))   j.at("AgentHeight").get_to(c.AgentHeight);
    if (j.contains("AgentMaxClimb")) j.at("AgentMaxClimb").get_to(c.AgentMaxClimb);
}

inline void to_json(nlohmann::json& j, const NavMeshConfigComponent& t) {
    j = nlohmann::json{
        {"CellSize",      t.CellSize},
        {"CellHeight",    t.CellHeight},
        {"AgentMaxSlope", t.AgentMaxSlope},
        {"TileSize",      t.TileSize},
        {"MaxObstacles",  t.MaxObstacles}
    };
    nlohmann::json classes = nlohmann::json::array();
    for (uint8_t i = 0; i < t.ClassCount && i < kMaxNavClasses; ++i) classes.push_back(t.Classes[i]);
    j["Classes"] = std::move(classes);
}
inline void from_json(const nlohmann::json& j, NavMeshConfigComponent& t) {
    if (j.contains("CellSize"))      j.at("CellSize").get_to(t.CellSize);
    if (j.contains("CellHeight"))    j.at("CellHeight").get_to(t.CellHeight);
    if (j.contains("AgentMaxSlope")) j.at("AgentMaxSlope").get_to(t.AgentMaxSlope);
    if (j.contains("TileSize"))      j.at("TileSize").get_to(t.TileSize);
    if (j.contains("MaxObstacles"))  j.at("MaxObstacles").get_to(t.MaxObstacles);

    if (j.contains("Classes") && j.at("Classes").is_array() && !j.at("Classes").empty()) {
        const auto& arr = j.at("Classes");
        const size_t n = std::min<size_t>(arr.size(), kMaxNavClasses);
        for (size_t i = 0; i < n; ++i) t.Classes[i] = arr.at(i).get<NavClassConfig>();
        t.ClassCount = static_cast<uint8_t>(n);
    } else {
        t.Classes[0] = NavClassConfig{};
        t.ClassCount = 1;
    }
}

// ----- world.json top-level "Environment" block -----

// Build the "Environment" object value (NOT wrapped — assign to root["Environment"]).
inline nlohmann::json BuildEnvironmentJson(const FogComponent& fog,
                                           const SkyComponent& sky,
                                           const DayNightConfigComponent& dayNight,
                                           const NavMeshConfigComponent& navmesh) {
    return nlohmann::json{
        {"Fog", fog},
        {"Sky", sky},
        {"DayNight", dayNight},
        {"NavMeshConfig", navmesh}};
}

// Parsed result of a root document's "Environment" block. Each Has* flag is true
// only if that sub-object was present; the corresponding value is otherwise left
// at its default. Absent "Environment" (old files) => all flags false.
struct EnvironmentData {
    bool HasFog = false;
    bool HasSky = false;
    bool HasDayNight = false;
    bool HasNavMeshConfig = false;
    FogComponent Fog;
    SkyComponent Sky;
    DayNightConfigComponent DayNight;
    NavMeshConfigComponent NavMeshConfig;
};

inline EnvironmentData ParseEnvironmentJson(const nlohmann::json& root) {
    EnvironmentData e;
    if (!root.contains("Environment")) return e;
    const auto& env = root.at("Environment");
    if (env.contains("Fog"))           { e.Fog           = env.at("Fog").get<FogComponent>();                  e.HasFog = true; }
    if (env.contains("Sky"))           { e.Sky           = env.at("Sky").get<SkyComponent>();                  e.HasSky = true; }
    if (env.contains("DayNight"))      { e.DayNight      = env.at("DayNight").get<DayNightConfigComponent>();  e.HasDayNight = true; }
    if (env.contains("NavMeshConfig")) { e.NavMeshConfig = env.at("NavMeshConfig").get<NavMeshConfigComponent>(); e.HasNavMeshConfig = true; }
    return e;
}
