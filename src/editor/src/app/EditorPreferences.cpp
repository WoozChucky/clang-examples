#include "EditorPreferences.h"

#include <fstream>

#include "lib.h" // SM_WARN / SM_TRACE

using json = nlohmann::json;

namespace EditorPreferences {

bool Load(const std::string& path, EditorCameraState& camera) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        SM_TRACE("EditorPreferences: '%s' not found; using defaults", path.c_str());
        return true;
    }
    json j;
    try {
        ifs >> j;
    } catch (const std::exception& ex) {
        SM_WARN("EditorPreferences: failed to parse '%s': %s", path.c_str(), ex.what());
        return false;
    }
    PrefsFromJson(j, GetCullingSettings(), GetDebugDrawSettings(), camera);
    SM_TRACE("EditorPreferences: loaded '%s'", path.c_str());
    return true;
}

bool Save(const std::string& path, const EditorCameraState& camera) {
    const json j = PrefsToJson(GetCullingSettings(), GetDebugDrawSettings(), camera);
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        SM_WARN("EditorPreferences: could not open '%s' for writing", path.c_str());
        return false;
    }
    ofs << j.dump(4);
    if (!ofs.good()) {
        SM_WARN("EditorPreferences: write to '%s' failed", path.c_str());
        return false;
    }
    return true;
}

} // namespace EditorPreferences
