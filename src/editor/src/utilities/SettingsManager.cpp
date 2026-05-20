#include "SettingsManager.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

#include "lib.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {
    std::string ToLower(std::string_view s) {
        std::string out(s);
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }
}

namespace SettingsManager {

RendererAPI ParseBackend(std::string_view name) {
    const std::string n = ToLower(name);
    if (n == "vulkan"    || n == "vk")        return RendererAPI::Vulkan;
    if (n == "directx12" || n == "dx12")      return RendererAPI::DirectX12;
    if (n == "directx11" || n == "dx11")      return RendererAPI::DirectX11;
    return RendererAPI::Invalid;
}

const char* BackendToString(RendererAPI api) {
    switch (api) {
        case RendererAPI::Vulkan:    return "vulkan";
        case RendererAPI::DirectX12: return "directx12";
        case RendererAPI::DirectX11: return "directx11";
        case RendererAPI::Invalid:   return "invalid";
    }
    return "invalid";
}

bool Load(const std::string& filepath, ApplicationSettings* out) {
    if (!out) return false;

    std::ifstream ifs(filepath, std::ios::binary);
    if (!ifs.is_open()) {
        SM_TRACE("SettingsManager: '%s' not found; using defaults", filepath.c_str());
        return true;
    }

    json j;
    try {
        ifs >> j;
    } catch (const std::exception& ex) {
        SM_WARN("SettingsManager: failed to parse '%s': %s", filepath.c_str(), ex.what());
        return false;
    }

    if (j.contains("renderer") && j["renderer"].is_object()) {
        const auto& jr = j["renderer"];
        if (jr.contains("backend") && jr["backend"].is_string()) {
            const std::string backendStr = jr["backend"].get<std::string>();
            const RendererAPI parsed = ParseBackend(backendStr);
            if (parsed == RendererAPI::Invalid) {
                SM_WARN("SettingsManager: unknown renderer.backend '%s'; keeping default '%s'",
                        backendStr.c_str(), BackendToString(out->Backend));
            } else {
                out->Backend = parsed;
            }
        }
    }

    if (j.contains("window") && j["window"].is_object()) {
        const auto& jw = j["window"];
        if (jw.contains("width")  && jw["width"].is_number_unsigned())  out->windowWidth   = jw["width"].get<uint32_t>();
        if (jw.contains("height") && jw["height"].is_number_unsigned()) out->windowHeight  = jw["height"].get<uint32_t>();
        if (jw.contains("vsync")  && jw["vsync"].is_boolean())          out->vsyncEnabled  = jw["vsync"].get<bool>();
    }

    SM_TRACE("SettingsManager: loaded '%s' (backend=%s, %ux%u, vsync=%d)",
             filepath.c_str(), BackendToString(out->Backend),
             out->windowWidth, out->windowHeight, out->vsyncEnabled ? 1 : 0);
    return true;
}

bool Save(const std::string& filepath, const ApplicationSettings& settings) {
    json j;
    j["version"]               = SETTINGS_VERSION;
    j["renderer"]["backend"]   = BackendToString(settings.Backend);
    j["window"]["width"]       = settings.windowWidth;
    j["window"]["height"]      = settings.windowHeight;
    j["window"]["vsync"]       = settings.vsyncEnabled;

    std::ofstream ofs(filepath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        SM_WARN("SettingsManager: could not open '%s' for writing", filepath.c_str());
        return false;
    }

    ofs << j.dump(4);
    if (!ofs.good()) {
        SM_WARN("SettingsManager: write to '%s' failed", filepath.c_str());
        return false;
    }

    SM_TRACE("SettingsManager: saved '%s'", filepath.c_str());
    return true;
}

} // namespace SettingsManager
