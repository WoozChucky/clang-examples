# Renderer Backend Selection (Phase A) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the user persist a renderer backend (DirectX 12 / Vulkan) across editor launches via `editor_settings.json`, override it per-run via `--backend=...`, and change it via an ImGui Settings menu that prompts to restart.

**Architecture:** Add a stateless `SettingsManager` namespace (free `Load`/`Save` functions, mirroring `WorldManager`) that reads/writes a JSON file next to the executable. `ApplicationSettings` gains a `RendererAPI Backend` field — the single source of truth. `main.cpp` parses a CLI override; `Application::Init` loads the file, applies the CLI override in-memory only, and constructs `RenderThread` from `Settings.Backend`. A `Settings` top-level menu in ImGui exposes a combo + Apply button; Apply writes the file and raises a restart banner.

**Tech Stack:** C++23, CMake presets, MSVC via VS2026, nlohmann/json (already in tree via `WorldManager`), ImGui (already used by editor), NVRHI backends (existing `RendererBackendDX12`, `RendererBackendVulkan`).

**Spec:** `docs/superpowers/specs/2026-05-20-renderer-backend-selection-design.md` — read first.

> **Testing note:** Per spec §"Testing", no automated tests are added in this phase. Each task ends with a build check; the final task runs the manual smoke matrix from the spec.

---

## File Structure

**Created:**
- `src/editor/src/utilities/SettingsManager.h` — public namespace API (`Load`, `Save`, `ParseBackend`, `BackendToString`).
- `src/editor/src/utilities/SettingsManager.cpp` — JSON I/O + parsing implementations.

**Modified:**
- `src/common/include/lib.h` — TODO comment on the `DirectX11` enumerator.
- `src/common/include/ApplicationContext.h` — add `RendererAPI Backend` field to `ApplicationSettings`.
- `src/editor/src/main.cpp` — CLI parsing, pass `std::optional<RendererAPI>` to `Application::Init`.
- `src/editor/src/core/Application.h` — `Init` signature takes optional override.
- `src/editor/src/core/Application.cpp` — load settings, apply CLI override, drop hardcoded Vulkan.
- `src/editor/src/rendering/imgui/ImGuiRenderer.h` — new member state (`m_PendingBackend`, `m_RestartRequired`, `m_SaveError`).
- `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` — `Settings` menu + restart banner.
- `src/editor/CMakeLists.txt` — add `SettingsManager.cpp` to sources.

---

## Build commands

Reused throughout. The active configure preset on this machine is `msvc-win64-vs2026-community`:

- Configure (only needed if CMake files change): `cmake --preset msvc-win64-vs2026-community`
- Build editor only: `cmake --build --preset msvc-win64-vs2026-community --target editor`
- Build everything in scope: `cmake --build --preset msvc-win64-vs2026-community --target ecs editor game test_ecs`
- Run editor: `./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe`
- Run editor with CLI: `./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe --backend=vulkan`

`runtime.exe` is currently broken on `main` (unrelated, pre-existing). Do not try to build it.

---

## Task 1: Add `Backend` field to `ApplicationSettings` and mark DX11 as TODO

This is the smallest, safest first step. The editor still launches under the existing hardcoded Vulkan path; only the data model grows.

**Files:**
- Modify: `src/common/include/ApplicationContext.h`
- Modify: `src/common/include/lib.h`

- [ ] **Step 1: Add `Backend` to `ApplicationSettings`**

Edit `src/common/include/ApplicationContext.h`. Replace the `ApplicationSettings` struct:

```cpp
struct ApplicationSettings {
    RendererAPI Backend    = RendererAPI::DirectX12;
    uint32_t    windowWidth   = 1920;
    uint32_t    windowHeight  = 1080;
    bool        vsyncEnabled  = true;
};
```

If the file does not already pull in `lib.h` (which defines `RendererAPI`), add `#include "lib.h"` at the top alongside the existing includes. `ECS.h` is already included from `ApplicationContext.h` and typically pulls `lib.h` transitively — only add the include if the build fails.

- [ ] **Step 2: Mark `DirectX11` as not implemented**

Edit `src/common/include/lib.h` around the `RendererAPI` enum:

```cpp
enum class RendererAPI : uint8_t {
    Invalid,
    DirectX12,
    DirectX11,  // TODO: DirectX11 backend not implemented yet
    Vulkan,
};
```

- [ ] **Step 3: Build the editor**

Run:

```
cmake --build --preset msvc-win64-vs2026-community --target editor
```

Expected: clean build, no warnings about uninitialized enum.

- [ ] **Step 4: Commit**

```bash
git add src/common/include/ApplicationContext.h src/common/include/lib.h
git commit -m "Add Backend field to ApplicationSettings (default DX12)"
```

---

## Task 2: Create `SettingsManager.h`

Public namespace API only. Implementation comes next.

**Files:**
- Create: `src/editor/src/utilities/SettingsManager.h`

- [ ] **Step 1: Write the header**

Create `src/editor/src/utilities/SettingsManager.h` with the following content:

```cpp
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
```

- [ ] **Step 2: Header should compile standalone**

There is no source file yet; the header alone won't trigger any build target. Move on.

- [ ] **Step 3: Commit**

```bash
git add src/editor/src/utilities/SettingsManager.h
git commit -m "Add SettingsManager namespace header"
```

---

## Task 3: Implement `SettingsManager.cpp`

JSON I/O + parsing. Mirrors the `WorldManager.cpp` pattern (nlohmann/json, `SM_WARN`/`SM_TRACE` logging via `lib.h`).

**Files:**
- Create: `src/editor/src/utilities/SettingsManager.cpp`
- Modify: `src/editor/CMakeLists.txt`

- [ ] **Step 1: Write the implementation**

Create `src/editor/src/utilities/SettingsManager.cpp` with the following content:

```cpp
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
```

- [ ] **Step 2: Wire into editor CMakeLists**

Edit `src/editor/CMakeLists.txt`. Locate the existing utilities source block:

```cmake
    src/utilities/MeshLoader.cpp
    src/utilities/MaterialLoader.cpp
    src/utilities/WorldManager.cpp
```

Add `SettingsManager.cpp` immediately after `WorldManager.cpp`:

```cmake
    src/utilities/MeshLoader.cpp
    src/utilities/MaterialLoader.cpp
    src/utilities/WorldManager.cpp
    src/utilities/SettingsManager.cpp
```

- [ ] **Step 3: Re-configure**

CMakeLists changed → re-run configure:

```
cmake --preset msvc-win64-vs2026-community
```

Expected: configure completes with no errors. New source file is picked up.

- [ ] **Step 4: Build the editor**

```
cmake --build --preset msvc-win64-vs2026-community --target editor
```

Expected: clean build. `SettingsManager.cpp` compiles. No new warnings.

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/utilities/SettingsManager.cpp src/editor/CMakeLists.txt
git commit -m "Implement SettingsManager: JSON load/save + backend parse"
```

---

## Task 4: Update `Application::Init` to take an optional CLI override and load settings

Wire the data flow without touching `main.cpp` yet. After this task the editor still launches under hardcoded behavior (no CLI is parsed, but `Init` now loads `editor_settings.json` and uses `Settings.Backend`).

**Files:**
- Modify: `src/editor/src/core/Application.h`
- Modify: `src/editor/src/core/Application.cpp`

- [ ] **Step 1: Update `Application.h`**

Edit `src/editor/src/core/Application.h`. Add `<optional>` include and change the `Init` signature:

```cpp
#pragma once
#include <optional>

#include "PlatformThread.h"
#include "GameThread.h"
#include "RenderThread.h"

class Application {
public:
    Application() = default;
    ~Application() = default;

    // backendOverride: if set, replaces the persisted Settings.Backend
    // for this run only (no disk write).
    bool Init(std::optional<RendererAPI> backendOverride = std::nullopt);

    void Run();

private:
    std::shared_ptr<ApplicationContext> m_AppContext;

    // Main thread
    std::unique_ptr<PlatformThread>     m_PlatformThread;

    std::unique_ptr<GameThread>         m_GameThread;
    std::thread m_ThreadGame;

    std::unique_ptr<RenderThread>       m_RenderThread;
    std::thread m_ThreadRender;
};
```

- [ ] **Step 2: Update `Application.cpp`**

Replace the body of `src/editor/src/core/Application.cpp` (keep `Run()` intact):

```cpp
#include "Application.h"

#include "lib.h"
#include "utilities/SettingsManager.h"

bool Application::Init(std::optional<RendererAPI> backendOverride) {
    m_AppContext = std::make_shared<ApplicationContext>();

    // Load persisted settings (file may not exist; defaults stay in place).
    SettingsManager::Load(SettingsManager::DEFAULT_SETTINGS_PATH, &m_AppContext->Settings);

    // CLI override wins for this run; never written to disk.
    if (backendOverride.has_value()) {
        m_AppContext->Settings.Backend = *backendOverride;
        SM_TRACE("Application: CLI override → backend=%s",
                 SettingsManager::BackendToString(*backendOverride));
    }

    if (m_AppContext->Settings.Backend == RendererAPI::Invalid) {
        SM_ERROR("Application: resolved backend is Invalid; aborting");
        return false;
    }

    m_PlatformThread = std::make_unique<PlatformThread>(m_AppContext);
    if (!m_PlatformThread->Init()) {
        return false;
    }

    m_GameThread = std::make_unique<GameThread>(m_AppContext);
    m_RenderThread = std::make_unique<RenderThread>(
        m_AppContext,
        m_PlatformThread->GetWindow(),
        m_AppContext->Settings.Backend);
    return true;
}

void Application::Run() {
    // (existing body, unchanged)
}
```

Important: do not delete the existing `Run()` body — keep its full implementation as-is. Only the `Init` body and the includes change.

For the `Run()` body, reuse the contents currently in the file (you should not be removing any line of the existing `Run()` function).

- [ ] **Step 3: Build the editor**

```
cmake --build --preset msvc-win64-vs2026-community --target editor
```

Expected: clean build. No new warnings.

- [ ] **Step 4: Smoke run (no CLI yet)**

Delete any existing `editor_settings.json` to start clean:

```
rm -f out/build/msvc-win64-vs2026-community/bin/Debug/editor_settings.json
```

Then launch the editor:

```
./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe
```

Expected:
- Log shows `SettingsManager: 'editor_settings.json' not found; using defaults`.
- Editor launches under **DirectX 12** (the new default), not Vulkan as before.
- Close the editor cleanly.

If the editor fails to start under DX12 on your machine (e.g. missing DX12 device), the expected MessageBox behavior is not yet wired (Task 5 area, but for now `Application::Init` returns false and the editor exits cleanly — that is acceptable for this task).

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/core/Application.h src/editor/src/core/Application.cpp
git commit -m "Load editor settings on startup; drop hardcoded Vulkan"
```

---

## Task 5: Parse `--backend=...` / `--help` in `main.cpp`

**Files:**
- Modify: `src/editor/src/main.cpp`

- [ ] **Step 1: Add CLI parsing and pass the override to `Application::Init`**

Replace the body of `int main()` in `src/editor/src/main.cpp` and add the necessary includes. Keep the existing `platform_debug_break` function and other prior includes intact; this step only modifies the includes and the `main` function.

```cpp
#include <optional>
#include <string>
#include <string_view>
// ... existing includes (algorithm, GLFW, atomic, chrono, etc.) stay ...
#include "alloc.h"
#include "lib.h"
#include "Application.h"
#include "utilities/SettingsManager.h"

// ... existing platform_debug_break(...) stays unchanged ...

namespace {
    void PrintUsage() {
        std::printf("Usage: editor.exe [--backend=<api>] [--help]\n"
                    "\n"
                    "  --backend=<api>   Override the persisted renderer backend for this run\n"
                    "                    only. Does NOT modify editor_settings.json.\n"
                    "                    Valid values: vulkan, vk, directx12, dx12.\n"
                    "                    (directx11/dx11 is reserved but the backend is not\n"
                    "                    implemented yet.)\n"
                    "  --help, -h        Print this help and exit.\n");
    }

    // Returns:
    //   - std::nullopt + parseOk=true  → no override on the command line.
    //   - RendererAPI value + parseOk=true → override resolved.
    //   - std::nullopt + parseOk=false → bad CLI (usage already printed).
    //   - std::nullopt + helpRequested=true → --help was passed.
    struct CliResult {
        std::optional<RendererAPI> override_;
        bool parseOk = true;
        bool helpRequested = false;
    };

    CliResult ParseCli(int argc, char** argv) {
        CliResult r;
        for (int i = 1; i < argc; ++i) {
            std::string_view arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                r.helpRequested = true;
                continue;
            }
            constexpr std::string_view kBackend = "--backend=";
            if (arg.substr(0, kBackend.size()) == kBackend) {
                const std::string_view value = arg.substr(kBackend.size());
                const RendererAPI parsed = SettingsManager::ParseBackend(value);
                if (parsed == RendererAPI::Invalid) {
                    std::printf("Error: invalid --backend value '%.*s'.\n\n",
                                static_cast<int>(value.size()), value.data());
                    r.parseOk = false;
                    return r;
                }
                r.override_ = parsed; // last occurrence wins
                continue;
            }
            std::printf("Error: unknown argument '%.*s'.\n\n",
                        static_cast<int>(arg.size()), arg.data());
            r.parseOk = false;
            return r;
        }
        return r;
    }
}

int main(int argc, char** argv) {
    std::atexit([](){ DumpAllocations(); });

    SM_TRACE("Working Directory: %s", std::filesystem::current_path().string().c_str());

    const CliResult cli = ParseCli(argc, argv);
    if (!cli.parseOk) {
        PrintUsage();
        return 1;
    }
    if (cli.helpRequested) {
        PrintUsage();
        return 0;
    }

    Application app;
    if (!app.Init(cli.override_)) {
        SM_ERROR("Application initialization failed!");
        MessageBoxA(nullptr,
                    "Renderer initialization failed.\n\n"
                    "Edit editor_settings.json next to editor.exe, or relaunch with\n"
                    "  --backend=vulkan\n"
                    "  --backend=directx12\n",
                    "Editor — startup failure",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return -1;
    }

    app.Run();

    SM_TRACE("Shutdown complete.");
    return 0;
}
```

- [ ] **Step 2: Build the editor**

```
cmake --build --preset msvc-win64-vs2026-community --target editor
```

Expected: clean build.

- [ ] **Step 3: Smoke run `--help`**

```
./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe --help
```

Expected: usage text printed, exit code 0, no editor window.

- [ ] **Step 4: Smoke run `--backend=vk`**

```
./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe --backend=vk
```

Expected: editor launches under **Vulkan** (override active), log shows `Application: CLI override → backend=vulkan`. Close.

- [ ] **Step 5: Smoke run `--backend=foo`**

```
./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe --backend=foo
```

Expected: prints `Error: invalid --backend value 'foo'.` then usage; exit code 1; no editor window.

- [ ] **Step 6: Commit**

```bash
git add src/editor/src/main.cpp
git commit -m "Add --backend and --help CLI flags to editor.exe"
```

---

## Task 6: Add `Settings` top-level menu with backend combo + Apply

ImGui plumbing. After this task the menu exists, the combo shows the current backend, and `Apply` writes the JSON file but the restart banner is still missing (Task 7).

**Files:**
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.h`
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`

- [ ] **Step 1: Add member state to `ImGuiRenderer.h`**

In `src/editor/src/rendering/imgui/ImGuiRenderer.h`, add to the existing `private:` member list (alongside `m_MeshPreviewState`):

```cpp
    // Settings menu state — pending backend selection until user clicks Apply.
    // Initialized lazily on first menu open from m_AppContext->Settings.Backend.
    RendererAPI m_PendingBackend = RendererAPI::Invalid;
    bool        m_PendingBackendInitialized = false;
    bool        m_RestartRequired = false;   // Task 7
    std::string m_SettingsSaveError;          // empty when no error
```

Add `#include "lib.h"` at the top of the header next to the existing includes (for `RendererAPI`). If `lib.h` is already pulled in transitively this is harmless.

- [ ] **Step 2: Add `Settings` menu to the main menu bar**

In `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`, add an include near the existing includes:

```cpp
#include "utilities/SettingsManager.h"
```

Locate the `if (ImGui::BeginMainMenuBar())` block (around line 369). Immediately after the `About` menu's closing `ImGui::EndMenu();` and before `ImGui::EndMainMenuBar();`, insert the following block:

```cpp
            if (ImGui::BeginMenu("Settings"))
            {
                ImGui::TextDisabled("Renderer");
                ImGui::Separator();

                // Lazy-initialize pending choice from the current persisted setting.
                if (!m_PendingBackendInitialized) {
                    m_PendingBackend = m_AppContext->Settings.Backend;
                    m_PendingBackendInitialized = true;
                }

                const char* current = SettingsManager::BackendToString(m_PendingBackend);
                if (ImGui::BeginCombo("Backend", current))
                {
                    if (ImGui::Selectable("directx12", m_PendingBackend == RendererAPI::DirectX12)) {
                        m_PendingBackend = RendererAPI::DirectX12;
                    }
                    if (ImGui::Selectable("vulkan", m_PendingBackend == RendererAPI::Vulkan)) {
                        m_PendingBackend = RendererAPI::Vulkan;
                    }

                    // DirectX 11 — disabled; backend not implemented.
                    ImGui::BeginDisabled(true);
                    ImGui::Selectable("directx11 (not implemented)", false);
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip("DirectX 11 backend not implemented yet.");
                    }

                    ImGui::EndCombo();
                }

                const bool dirty = (m_PendingBackend != m_AppContext->Settings.Backend);
                ImGui::BeginDisabled(!dirty);
                if (ImGui::Button("Apply##SettingsBackendApply"))
                {
                    const RendererAPI previous = m_AppContext->Settings.Backend;
                    m_AppContext->Settings.Backend = m_PendingBackend;
                    if (SettingsManager::Save(SettingsManager::DEFAULT_SETTINGS_PATH,
                                              m_AppContext->Settings))
                    {
                        m_SettingsSaveError.clear();
                        m_RestartRequired = true;
                    }
                    else
                    {
                        // Revert in-memory change; no banner.
                        m_AppContext->Settings.Backend = previous;
                        m_SettingsSaveError = "Failed to save editor_settings.json";
                    }
                }
                ImGui::EndDisabled();

                if (!m_SettingsSaveError.empty()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                       "%s", m_SettingsSaveError.c_str());
                }

                ImGui::EndMenu();
            }
```

- [ ] **Step 3: Build the editor**

```
cmake --build --preset msvc-win64-vs2026-community --target editor
```

Expected: clean build.

- [ ] **Step 4: Smoke — Settings menu opens and combo shows current backend**

Launch the editor. Open `Settings → Renderer Backend`. Confirm:
- The combo's selected value matches the current persisted backend (DX12 if the file does not exist, otherwise whatever the file says).
- `directx11 (not implemented)` is greyed out and shows the tooltip on hover.
- `Apply` is greyed out unless the combo differs from `m_AppContext->Settings.Backend`.

Change the combo to `vulkan` and click `Apply`. The Apply button greys out (state matches now), and `editor_settings.json` appears (or is updated) in the editor's working directory (`out/build/msvc-win64-vs2026-community/bin/Debug/`). Inspect it:

```
cat out/build/msvc-win64-vs2026-community/bin/Debug/editor_settings.json
```

Expected JSON shape (formatting may differ slightly):

```json
{
    "renderer": { "backend": "vulkan" },
    "version": 1,
    "window": { "height": 1080, "vsync": true, "width": 1920 }
}
```

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/rendering/imgui/ImGuiRenderer.h src/editor/src/rendering/imgui/ImGuiRenderer.cpp
git commit -m "Add Settings menu with Renderer Backend combo + Apply"
```

---

## Task 7: Add the restart-required banner

`m_RestartRequired` is already set on a successful Apply (Task 6). Now render the banner.

**Files:**
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`

- [ ] **Step 1: Render the banner**

In `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`, locate the line directly after `ImGui::EndMainMenuBar();` (around line 408). Insert:

```cpp
        // Restart banner: shown after a successful renderer-backend Apply.
        if (m_RestartRequired) {
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            const float bannerHeight = 30.0f;
            // Position below the main menu bar.
            const ImVec2 pos(viewport->WorkPos.x, viewport->WorkPos.y);
            const ImVec2 size(viewport->WorkSize.x, bannerHeight);
            ImGui::SetNextWindowPos(pos);
            ImGui::SetNextWindowSize(size);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.95f, 0.78f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text,     ImVec4(0.10f, 0.10f, 0.10f, 1.0f));
            if (ImGui::Begin("##RestartBanner", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoResize     | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav))
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Restart editor to apply renderer changes.");
                ImGui::SameLine();
                if (ImGui::SmallButton("Dismiss##RestartBanner")) {
                    m_RestartRequired = false;
                }
            }
            ImGui::End();
            ImGui::PopStyleColor(2);
        }
```

(The viewport size is taken from `WorkPos`/`WorkSize` so the banner sits below the menu bar.)

- [ ] **Step 2: Build the editor**

```
cmake --build --preset msvc-win64-vs2026-community --target editor
```

Expected: clean build.

- [ ] **Step 3: Smoke — banner appears after Apply**

Launch editor. `Settings → Renderer Backend → vulkan → Apply`. A yellow banner appears at the top of the editor: `Restart editor to apply renderer changes.` with a `Dismiss` button. Click `Dismiss` → banner disappears. Apply a different backend again → banner re-appears.

- [ ] **Step 4: Commit**

```bash
git add src/editor/src/rendering/imgui/ImGuiRenderer.cpp
git commit -m "Render restart-required banner after Apply"
```

---

## Task 8: Run the full smoke matrix from the spec

This is the verification gate. Every row in the spec's smoke matrix must pass.

**Files:** none modified (testing only).

- [ ] **Step 1: Fresh-install path — no settings file**

```
rm -f out/build/msvc-win64-vs2026-community/bin/Debug/editor_settings.json
./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe
```

Expected: editor launches under **DirectX 12**; log shows `'editor_settings.json' not found; using defaults`. Use `Settings → Renderer Backend → Apply` once with DX12 → file appears. Close.

- [ ] **Step 2: File says vulkan**

Edit `editor_settings.json` (or use Apply) so `renderer.backend` = `"vulkan"`. Launch:

```
./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe
```

Expected: editor launches under **Vulkan**. Close.

- [ ] **Step 3: File says directx12**

Set `renderer.backend` = `"directx12"`. Launch.

Expected: editor launches under **DirectX 12**. Close.

- [ ] **Step 4: File says directx11**

Set `renderer.backend` = `"directx11"`. Launch.

Expected: `Renderer::Init` switch-case has no DX11 branch (see `src/editor/src/rendering/Renderer.cpp:20-26`); the renderer fails to initialize → `Application::Init` returns false → `main` shows the MessageBox `"Renderer initialization failed. Edit editor_settings.json or relaunch with --backend=..."` and exits. Verify the MessageBox appears and dismissing it closes the editor cleanly.

- [ ] **Step 5: File says metal (unknown value)**

Set `renderer.backend` = `"metal"`. Launch.

Expected: log shows `WARN ... unknown renderer.backend 'metal'; keeping default 'directx12'`. Editor launches under DX12. Close.

- [ ] **Step 6: Malformed JSON**

Write garbage to `editor_settings.json`:

```
echo "{not valid json" > out/build/msvc-win64-vs2026-community/bin/Debug/editor_settings.json
```

Launch the editor.

Expected: log shows `WARN ... failed to parse 'editor_settings.json': ...`. Defaults kept; editor launches under DX12. File is **not** auto-overwritten. Close. Restore a valid file via Apply (or delete and start fresh).

- [ ] **Step 7: CLI override does not modify file**

Ensure `editor_settings.json` says `directx12`. Run:

```
./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe --backend=vk
```

Expected: editor launches under Vulkan. Close. Inspect file — still says `directx12`.

- [ ] **Step 8: Invalid CLI value**

```
./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe --backend=foo
```

Expected: prints error + usage, exit code 1, no editor window.

- [ ] **Step 9: `--help`**

```
./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe --help
```

Expected: usage text, exit code 0, no editor window.

- [ ] **Step 10: Apply round-trip**

Launch editor. `Settings → Renderer Backend → vulkan → Apply` → banner appears. `Dismiss`. Close. Relaunch — comes up Vulkan, no banner. Inspect `editor_settings.json` — says `"vulkan"`.

- [ ] **Step 11: Apply fails on read-only directory (optional, machine-dependent)**

If the build directory can be made read-only (Windows: right-click → Properties → Read-only, or NTFS perms), do so, launch editor, and try Apply. Expected: red error text near the combo: `Failed to save editor_settings.json`. No banner. Combo reverts to the persisted value. Skip this step if read-only configuration is not convenient — it's the only non-required row.

- [ ] **Step 12: Commit any cosmetic fixes**

If steps 1-11 surface any minor fixes (typo, log noise, etc.), commit them as a tiny follow-up commit:

```bash
git add <files>
git commit -m "Polish renderer-backend selection after smoke matrix"
```

If everything passes cleanly with no fixes, no commit needed.

---

## Task 9: Wrap up

- [ ] **Step 1: Verify in-scope targets all build cleanly**

```
cmake --build --preset msvc-win64-vs2026-community --target ecs editor game test_ecs
```

Expected: clean build for all four targets.

- [ ] **Step 2: Run ECS tests (smoke for cross-target regressions)**

```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```

Expected: `All ECS tests passed.`

- [ ] **Step 3: Branch + PR**

```
git log --oneline origin/main..HEAD
```

If this work is on `main` directly, create a feature branch and push:

```
git checkout -b renderer-backend-selection
git push -u origin renderer-backend-selection
gh pr create --title "Renderer backend selection (Phase A): persist + CLI override" \
             --body "$(cat <<'EOF'
## Summary

Implements Phase A of the renderer backend selection feature:
- `editor_settings.json` next to the executable persists the chosen
  renderer (DirectX 12 default; Vulkan also supported).
- `editor.exe --backend=<api>` overrides the persisted value per-run
  without modifying the JSON file.
- New `Settings` menu in the main menu bar with a Renderer Backend
  combo and an Apply button; on successful Apply a restart-required
  banner is raised until dismissed.
- DirectX 11 is reserved in the enum and listed in the combo as
  `(not implemented)`. Backend implementation is out of scope.

Spec: `docs/superpowers/specs/2026-05-20-renderer-backend-selection-design.md`.

Phase B (runtime hot-swap) reuses the settings layer added here.

## Test plan

- [x] Fresh install (no settings file) → launches under DX12, file created on first Apply
- [x] File says vulkan → launches Vulkan
- [x] File says directx12 → launches DX12
- [x] File says directx11 → Application::Init fails; MessageBox shown; clean exit
- [x] File says metal → WARN logged, defaults to DX12
- [x] Malformed JSON → WARN logged, defaults to DX12, file untouched
- [x] `--backend=vk` over DX12 file → launches Vulkan, file unchanged
- [x] `--backend=foo` → usage printed, exit 1
- [x] `--help` → usage printed, exit 0
- [x] Apply round-trip → file rewritten, banner appears, dismiss works,
  relaunch picks up new backend
EOF
)"
```

- [ ] **Step 4: Hand off**

PR opened. Hand the URL back to the user; nothing else to do in this plan.

---

## Out-of-scope notes (for the implementer's awareness)

- **No automated tests.** Per the spec.
- **Window size and vsync are persisted but not editable** through ImGui. The fields ride along in the JSON file so the format is forward-compatible; UI for them is deferred.
- **`runtime.exe` is broken on `main`** for unrelated reasons (`PlatformContext::m_Input` missing). Do not attempt to build it as part of this plan.
