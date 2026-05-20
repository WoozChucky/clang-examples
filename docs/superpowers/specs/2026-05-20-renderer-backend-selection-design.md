# Renderer Backend Selection (Phase A — Startup Switch)

**Date:** 2026-05-20
**Status:** Draft — pending user review
**Scope:** Persist a user-selected renderer backend (DirectX 12 or Vulkan) and apply it at editor startup. CLI override available. ImGui menu lets the user change the backend and prompts to restart. Hot-swap at runtime is **Phase B** (separate spec).

## Motivation

`src/editor/src/core/Application.cpp:15` currently hardcodes `RendererAPI::Vulkan` when constructing the `RenderThread`. There is no way to run the editor under DirectX 12 without recompiling, and no settings persistence layer exists. This blocks driver/back-end comparisons during development and removes a switch users will want.

Phase A keeps the change minimal: one persisted setting, one CLI flag, one ImGui menu entry, and a restart prompt. Phase B (hot-swap without restart) is acknowledged as the eventual goal; this phase produces the settings plumbing and backend-selection map Phase B will reuse.

## Goals

1. The chosen backend persists across launches in `editor_settings.json` next to the executable.
2. A CLI flag `--backend=...` overrides the persisted choice for one run without modifying the file.
3. The ImGui `Settings` menu lets the user change the backend and writes the new value on `Apply`. A restart banner is shown afterward.
4. The change preserves today's behavior for users who have no settings file: on first launch, the editor uses the default backend (DirectX 12, see §"Default backend").
5. `runtime.exe` and `test_ecs.exe` are unaffected. `game.dll` has no awareness of these settings.

Non-goals: runtime hot-swap, settings UI for window size / vsync (struct fields persisted, but no editing UI in this phase), DirectX 11 backend implementation, settings file outside the executable's directory, automated tests.

## Default backend

DirectX 12 is the default when `editor_settings.json` is absent. Rationale: native on Windows, lower driver overhead than Vulkan on consumer NVIDIA/AMD drivers in current development setups, and the existing `RendererBackendDX12` is in-tree. Changing this requires editing one constant.

## Architecture

```
editor.exe startup:
  main(argc, argv)
    └─ parse --backend=... → std::optional<RendererAPI> cliOverride
    └─ Application::Init(cliOverride)
         ├─ SettingsManager::Load("editor_settings.json", &m_AppContext->Settings)
         │     (file missing → defaults kept: Backend = DirectX12, vsync = true, 1920x1080)
         ├─ if (cliOverride) m_AppContext->Settings.Backend = *cliOverride
         │     (CLI overrides in-memory only; JSON not rewritten)
         ├─ PlatformThread::Init
         ├─ GameThread
         └─ RenderThread(api = Settings.Backend)   ← was hardcoded Vulkan

Runtime (ImGui main menu bar):
  Settings menu
    └─ Renderer Backend combo (DX12 / Vulkan, DX11 disabled w/ tooltip)
         + Apply button
              └─ writes editor_settings.json
              └─ raises a "Please restart" banner until Dismiss

Shutdown:  nothing extra (settings already persisted on Apply).
```

Single source of truth: `ApplicationContext::Settings`. Disk is just persistence; all consumers read the struct. `SettingsManager` is a stateless I/O helper, modeled on `WorldManager`.

## Components

### New: `src/editor/src/utilities/SettingsManager.{h,cpp}`

```cpp
// SettingsManager.h
namespace SettingsManager {
    constexpr auto DEFAULT_SETTINGS_PATH = "editor_settings.json";
    constexpr uint32_t SETTINGS_VERSION = 1;

    // Missing file → returns true, leaves `out` untouched (defaults).
    // Parse error → returns false, leaves `out` untouched, logs warning.
    bool Load(const std::string& filepath, ApplicationSettings* out);
    bool Save(const std::string& filepath, const ApplicationSettings& settings);

    RendererAPI ParseBackend(std::string_view name);   // "vulkan"/"vk", "dx12"/"directx12"
    const char* BackendToString(RendererAPI api);      // round-trip with ParseBackend
}
```

### JSON file shape

```json
{
  "version": 1,
  "renderer": { "backend": "directx12" },
  "window": { "width": 1920, "height": 1080, "vsync": true }
}
```

Rules:
- Unknown keys are silently ignored.
- Missing keys leave the corresponding field in `out` at its current value (i.e. the default already set in `ApplicationSettings`).
- An unknown `renderer.backend` value logs a warning and leaves `out->Backend` unchanged.
- `version` is informational in this phase; not enforced. Future versions may use it for migration.

### Modified: `src/common/include/ApplicationContext.h`

```cpp
struct ApplicationSettings {
    RendererAPI Backend    = RendererAPI::DirectX12;  // NEW
    uint32_t windowWidth   = 1920;
    uint32_t windowHeight  = 1080;
    bool     vsyncEnabled  = true;
};
```

### Modified: `src/common/include/lib.h`

Add a `// TODO: DirectX11 backend not implemented yet` comment next to the `DirectX11` enumerator. UI greys it out; CLI rejects it as invalid; loaded JSON value triggers init failure handled per §"Error handling".

### Modified: `src/editor/src/main.cpp`

Parse CLI flags before constructing `Application`. No third-party library. Loop over `argv[1..argc-1]`:

- `--help` / `-h` → print usage and valid backend names, `return 0`.
- `--backend=<name>` → parse via `SettingsManager::ParseBackend`. Invalid name → print error + usage, `return 1`. Multiple occurrences: last one wins.
- Unknown flag → print error + usage, `return 1`.

Pass the resolved `std::optional<RendererAPI>` to `Application::Init`.

### Modified: `src/editor/src/core/Application.{h,cpp}`

```cpp
bool Application::Init(std::optional<RendererAPI> backendOverride = std::nullopt);
```

Sequence inside `Init`:

1. Construct `m_AppContext`.
2. `SettingsManager::Load(SettingsManager::DEFAULT_SETTINGS_PATH, &m_AppContext->Settings)`. A `false` return logs a warning and leaves defaults in place; the editor still launches. The malformed file is preserved (not auto-overwritten) until a successful Apply rewrites it.
3. If `backendOverride` is set, write it to `m_AppContext->Settings.Backend`. No disk write.
4. Construct `PlatformThread`, `GameThread`, then `RenderThread(api = m_AppContext->Settings.Backend)` (drop hardcoded `RendererAPI::Vulkan`).

The hardcoded literal at `Application.cpp:15` is the only behavior change site for the launch path.

### Modified: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`

Add a top-level `Settings` menu to the existing main menu bar:

```
Settings
└── Renderer Backend
    ├── ○ DirectX 12       (combo)
    ├── ○ Vulkan
    └── ○ DirectX 11       (disabled, tooltip "Not implemented yet")
    [Apply]                (enabled only when pendingBackend != Settings.Backend)
```

State stored on the `ImGuiRenderer` instance:
- `RendererAPI m_PendingBackend` (initialized from `Settings.Backend` on first show / when reset).
- `bool m_RestartRequired = false`.

Apply button:
1. `m_AppContext->Settings.Backend = m_PendingBackend;`
2. `bool ok = SettingsManager::Save(SettingsManager::DEFAULT_SETTINGS_PATH, m_AppContext->Settings);`
3. If `ok` → `m_RestartRequired = true`. Else → inline ImGui error text near the combo, `Settings.Backend` reverted to its pre-Apply value, no banner.

While `m_RestartRequired` is true, render a yellow banner immediately below the menu bar: `Restart editor to apply renderer changes.` with a `Dismiss` button. Dismiss clears the flag; the next launch picks up the new backend regardless.

### Modified: `src/editor/CMakeLists.txt`

Add `src/utilities/SettingsManager.cpp` to the editor source list. No new third-party dependency (nlohmann/json already in tree via `WorldManager`).

## Error handling

| Source | Behavior |
|---|---|
| `editor_settings.json` missing | `Load` returns true, `out` keeps defaults. INFO log. First Apply creates the file. |
| Malformed JSON | `Load` returns false. WARN log with parse error. Defaults are kept; editor launches. The malformed file is preserved until a successful Apply rewrites it. |
| Unknown `renderer.backend` value | WARN log; `out->Backend` unchanged (default). Editor launches under default. |
| `--backend=<invalid>` | Print error + usage, exit 1. |
| `--help` / `-h` | Print usage, exit 0. |
| Multiple `--backend=` flags | Last one wins. Documented in `--help`. |
| Chosen backend fails to init | `RenderThread::Init` returns false → `Application::Init` returns false → MessageBox: `Renderer initialization failed: <api>. Edit editor_settings.json or pass --backend=... to change.` `main` returns -1. |
| `Save` fails on Apply (disk full, read-only) | Inline ImGui error near the combo. `Settings.Backend` reverted in-memory. No banner. No disk modification. |
| Two editor.exe instances writing concurrently | Out of scope. Last write wins. |

### Invariants

- After `Application::Init` returns true, `ApplicationContext::Settings.Backend` is never `Invalid`.
- A CLI run never causes a disk write.
- `editor_settings.json` always reflects the last *successful* Apply — never a partial / in-flight value.
- An unknown enumerator value never reaches `Renderer::Init` (filtered at load / Apply).

## Testing

No automated tests in this phase. Settings I/O is thin and the meaningful failures are environmental (drivers, permissions, missing files). Manual smoke matrix:

| Case | Expected |
|---|---|
| No settings file | Launches DX12. Apply once → file appears. |
| File says `"vulkan"` | Launches Vulkan. |
| File says `"directx12"` | Launches DX12. |
| File says `"directx11"` | Init fails, MessageBox; recover via JSON edit / CLI. |
| File says `"metal"` | WARN logged, falls back to default DX12, launches. |
| Malformed JSON | WARN logged, defaults kept, editor launches. File untouched. |
| `--backend=vk` over a DX12 file | Launches Vulkan. File unchanged after exit. |
| `--backend=foo` | Usage printed, exit 1. |
| `--help` | Usage printed, exit 0. |
| Apply DX12 → Vulkan in menu | File rewritten. Yellow restart banner appears until Dismiss. |
| Apply, Dismiss, quit, relaunch | Comes up with new backend. Banner gone. |
| Apply fails (read-only dir) | ImGui inline error. No banner. File unchanged. |
| Vulkan-only machine, file says DX12 | MessageBox on init fail; clean exit. |

`test_ecs.exe` is unaffected — no ECS surface touched.

## Rollback

Reverting this change requires only:
- Delete `src/editor/src/utilities/SettingsManager.{h,cpp}` (and its CMakeLists entry).
- Restore the hardcoded `RendererAPI::Vulkan` in `Application.cpp`.
- Remove the `Settings` menu block in `ImGuiRenderer.cpp`.
- Remove the `Backend` field from `ApplicationSettings`.
- Optional: leave the CLI parser stub or strip it.

No data migration. `editor_settings.json` files in the wild become orphaned but cause no harm — `WorldManager` is independent.

## Out of scope (deferred to Phase B)

- Runtime swap of the renderer backend without restart.
- Recreation of GPU resources (textures, buffers, pipelines, ImGui backend) on backend change.
- GPU drain / device-idle barrier ahead of teardown.
- Window/swapchain re-creation against a different `nvrhi::IDevice`.
- Implementation of `RendererBackendDX11`.
- Settings UI for window size and vsync.
- Settings file location outside the executable directory (e.g. `%APPDATA%`).
- Automated tests for SettingsManager.
