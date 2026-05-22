#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "ApplicationContext.h"

namespace SettingsManager {
    // Settings persist in editor_settings.json next to the executable.
    // Unknown keys are ignored on load so the file format can evolve.
    constexpr auto     DEFAULT_SETTINGS_PATH = "editor_settings.json";
    constexpr uint32_t SETTINGS_VERSION      = 1;

    // Reads JSON at `filepath` and merges fields into `*out`.
    // - Missing file → returns true, leaves `*out` untouched (caller-supplied defaults stay).
    // - Parse error / malformed JSON → returns false, logs WARN, leaves `*out` untouched.
    // - Unknown `renderer.backend` value → WARN logged, `out->Backend` not changed.
    // - Unknown JSON keys → silently ignored.
    bool Load(const std::string& filepath, ApplicationSettings* out);

    // Serializes `settings` and writes JSON to `filepath`.
    // Returns false on any I/O failure (ofstream open / write error).
    bool Save(const std::string& filepath, const ApplicationSettings& settings);

    // Parse "vulkan"/"vk", "directx12"/"dx12", "directx11"/"dx11".
    // Case-insensitive. Returns RendererAPI::Invalid on unknown input.
    RendererAPI ParseBackend(std::string_view name);

    // Returns canonical name: "vulkan", "directx12", "directx11", or "invalid".
    const char* BackendToString(RendererAPI api);
}
