# Game Library Hot-Reload Design

**Date:** 2026-05-19
**Status:** Draft — pending user review
**Scope:** Re-enable hot-reload of the `game` library for the `editor` target under the 3-thread model. Splits ECS code into a new `ecs.dll` so DLL boundaries are safe across game reloads.

## Motivation

The legacy `runtime` target supported hot-reload of `game.dll` via `LoadLibrary` + timestamped DLL copies. The current `editor` target links `game` statically — every code change forces a full editor restart, which costs roughly a 15-second iteration penalty per edit.

Constraints that make a naive port unsafe:

1. The `editor`'s 3-thread model means reload must coordinate with `GameThread`'s tick loop. Render and platform threads are unaffected (they don't call into game code).
2. `src/game/src/Game.cpp` currently reaches into editor-owned headers (`#include "../../editor/src/utilities/WorldManager.h"`). That coupling must be cut before `game` can be a SHARED library.
3. The new ECS uses copy-on-write via `std::shared_ptr<ComponentArray<T>>`. The control-block deleter for a `shared_ptr` lives in whichever translation unit instantiated the template. If `game.dll` instantiates `MutateArray<T>` locally and the resulting `shared_ptr` outlives `game.dll`'s load, the deleter dangles → crash on the next `~shared_ptr`.

This design solves all three.

## Goal

Make every change to `src/game/src/Game.cpp` reloadable without restarting the editor, while preserving:

1. The single-writer rule (only GameThread mutates master ECS).
2. ECS snapshot isolation (RenderThread reads survive reload).
3. Entity-handle stability across reload (no duplicate spawns).
4. Input state across reload (held keys do not appear released).
5. Build-system simplicity for developers (run `cmake --build --target game`; the editor auto-reloads).

Non-goals: hot-reload of `editor.exe` itself, of `ecs.dll`, of `Game.h` layout changes (those require full rebuild + restart). Non-Windows platforms. SEH crash isolation. ImGui-driven build invocation.

---

## Architecture

3-thread model unchanged (PlatformThread / GameThread / RenderThread, all communication via SPSC rings + Seqlock + atomic shared_ptr<const ECS>). The change touches `GameThread`'s game-update dispatch path and adds a new `ecs.dll` target.

### Target topology

```
ecs.dll       ← all ECS template instantiations + non-templated methods live here
              ← exports via ECS_API
              ← stays loaded for the entire editor process lifetime

editor.exe    ── links → ecs.dll, GameHeaders (Game.h type defs only), NVRHI, ImGui, ImGuizmo, assimp, freetype, nethost
              ── loads at runtime → game.dll via LoadLibrary
              ── owns GameState on GameThread's stack; passes &gameState to game.dll's GameUpdate

game.dll      ── links → ecs.dll, glm
              ── exports: GameGetVersion, GameUpdate, GameResize, GameExit
              ── unloaded and reloaded on Game.dll filesystem changes

test_ecs.exe  ── links → ecs.dll (gains direct test coverage for the shipped binary)
runtime.exe   ── unchanged
```

### GameThread dispatch change

```
Today:
  GameThread::RunLoop ──► GameUpdate(&gameState)                [static-linked]

After:
  GameThread owns:
    GameLibrary                                m_GameLib              // RAII wrapper around HMODULE + symbols
    std::atomic<bool>                          m_ReloadPending{false} // set by watcher thread
    std::unique_ptr<filewatch::FileWatch<...>> m_GameDllWatcher       // background watch thread

  GameThread::RunLoop ──► if m_ReloadPending.exchange(false) → m_GameLib.LoadOrReload(...)
                       ──► m_GameLib.Update(&gameState)         [indirect via function ptr]
```

### Threading invariants preserved

- GameThread is still single-writer to ECS. Game.dll's mutations happen from GameThread (game.dll's `GameUpdate` is invoked synchronously by GameThread).
- RenderThread reads via `shared_ptr<const ECS>` snapshot; immune to reload.
- PlatformThread input ring unaffected.

### Cross-DLL ABI safety (the core of Pattern Z)

Memory boundary issues come from having **two copies of the same template instantiated in different modules**. With `ecs.dll` as the canonical home, ECS templates instantiate exactly once. When `game.dll` calls `MutateArray<TransformComponent>()`:

1. Compiler sees `extern template class ECS_API ComponentArray<TransformComponent>;` in the header → does NOT instantiate locally.
2. Linker emits a call into `ecs.dll`'s exported symbol.
3. `std::make_shared<ComponentArray<TransformComponent>>(...)` inside the exported function executes in `ecs.dll`'s code → captured deleter points into `ecs.dll`.
4. When `game.dll` is `FreeLibrary`'d during reload, `ecs.dll` stays loaded. Any snapshot still holding the shared_ptr can release it safely — its deleter lives in `ecs.dll`.

`g_GameState->World.CreateEntity()` works regardless of export discipline because `CreateEntity` doesn't create any `shared_ptr` and doesn't capture any deleter. Its only effect on the heap is `std::vector<EntityId>::push_back` — same CRT heap, no cross-DLL lifetime issue. Inline-into-game.dll is safe for any method that doesn't create a shared_ptr.

ECS_API discipline is required only for methods that create `shared_ptr<ComponentArray<T>>`:
- `ComponentArray<T>::Clone()`
- `ComponentStore::MutateArray<T>`, `AddComponent<T>`, `RemoveComponent<T>`, `RemoveAllComponents`
- `ECS::AddComponent<T>`, `RemoveComponent<T>`, `MutateArray<T>`, `DestroyEntity`, `CreateSnapshot`

---

## Game.dll API contract

`Game.h` already declares the export surface; this section finalizes it.

```cpp
// src/game/include/Game.h

#pragma once
#include "ApplicationContext.h"
#include "ECS.h"
#include "Camera.h"

#ifdef _WIN32
  #define EXPORT_FN __declspec(dllexport)
#else
  #define EXPORT_FN
#endif

// Bump every time GameState layout or any export signature changes.
// Editor compares against compiled-in expected value at load time.
#define GAME_API_VERSION 1u

extern "C"
{
    EXPORT_FN uint32_t GameGetVersion();
    EXPORT_FN void     GameUpdate(GameState* state);
    EXPORT_FN void     GameResize(uint32_t width, uint32_t height);
    EXPORT_FN void     GameExit(GameState* state);
}

using GameGetVersionFunc = uint32_t(*)();
using GameUpdateFunc     = void(*)(GameState* state);
using GameResizeFunc     = void(*)(uint32_t width, uint32_t height);
using GameExitFunc       = void(*)(GameState* state);
```

Required exports (load fails on missing):
- `GameGetVersion()` — returns `GAME_API_VERSION`. Editor compares; mismatch rejects load.
- `GameUpdate(GameState*)` — single tick.

Optional exports (load succeeds on missing):
- `GameResize(uint32_t, uint32_t)` — invoked on window resize. Absent → silently dropped.
- `GameExit(GameState*)` — invoked once per process shutdown AND once per reload (on the outgoing module, before `FreeLibrary`). Absent → no shutdown hook.

`extern "C"` avoids name mangling so `GetProcAddress("GameUpdate")` finds the symbol. `GameState*` is a raw pointer; layout is governed by the header, which both editor and game compile against identically.

`GameState` is allocated **once** on GameThread's stack and survives reload. Game.dll only receives a pointer. After §"State migration" the file-static globals in game.cpp move into `GameState` so they survive too.

---

## `GameLibrary` class

RAII wrapper around `HMODULE` + resolved symbol pointers. Owned by `GameThread`. Single-threaded access.

### Files

- Create: `src/editor/src/threading/GameLibrary.h`
- Create: `src/editor/src/threading/GameLibrary.cpp`

### Interface

```cpp
#pragma once
#include <windows.h>
#include <string>
#include "Game.h"

class GameLibrary {
public:
    GameLibrary() = default;
    ~GameLibrary();

    GameLibrary(const GameLibrary&) = delete;
    GameLibrary& operator=(const GameLibrary&) = delete;
    GameLibrary(GameLibrary&&) = delete;
    GameLibrary& operator=(GameLibrary&&) = delete;

    /**
     * @brief Loads `sourceDllPath` via a timestamped copy. Resolves required
     *        and optional symbols. Validates GAME_API_VERSION. On reload,
     *        invokes prior module's GameExit(state) before FreeLibrary,
     *        then performs the swap.
     * @return true if a new module is now active; false if load/validation
     *         failed and the previous module (if any) is preserved.
     * @threading GameThread only.
     */
    bool LoadOrReload(const std::string& sourceDllPath, GameState* state);

    /** @brief True when a callable GameUpdate is installed. */
    bool IsValid() const { return m_pGameUpdate != nullptr; }

    /** @pre IsValid() must be true. */
    void Update(GameState* state) const { m_pGameUpdate(state); }

    /** @brief Invokes GameResize if exported; no-op otherwise. */
    void Resize(uint32_t width, uint32_t height) const {
        if (m_pGameResize) m_pGameResize(width, height);
    }

    /** @brief Invokes GameExit if exported, then frees current module. Idempotent. */
    void Unload(GameState* state);

private:
    HMODULE             m_Module          = nullptr;
    std::string         m_LoadedDllPath;
    GameUpdateFunc      m_pGameUpdate     = nullptr;
    GameResizeFunc      m_pGameResize     = nullptr;
    GameExitFunc        m_pGameExit       = nullptr;
    GameGetVersionFunc  m_pGameGetVersion = nullptr;
    uint64_t            m_ReloadCounter   = 0;
};
```

### LoadOrReload sequence

```cpp
bool GameLibrary::LoadOrReload(const std::string& sourceDllPath, GameState* state) {
    namespace fs = std::filesystem;

    // 1. Build timestamped copy path under same dir.
    const fs::path srcPath(sourceDllPath);
    const auto srcDir   = srcPath.parent_path().string();
    const auto baseName = srcPath.stem().string();
    const auto copyPath = MakeTimestampedCopyPath(srcDir, baseName, ++m_ReloadCounter);

    // 2. Copy source DLL to load-safe name. Fail soft if locked or missing.
    std::error_code ec;
    fs::copy_file(srcPath, copyPath, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        SM_WARN("GameLibrary: copy_file('%s' -> '%s') failed: %s",
                sourceDllPath.c_str(), copyPath.c_str(), ec.message().c_str());
        return false;
    }

    // 3. LoadLibrary on the copy.
    HMODULE newModule = LoadLibraryA(copyPath.c_str());
    if (!newModule) {
        SM_ERROR("GameLibrary: LoadLibraryA('%s') failed (GetLastError=%lu)",
                 copyPath.c_str(), GetLastError());
        fs::remove(copyPath, ec);
        return false;
    }

    // 4. Resolve required symbols.
    auto pVersion = reinterpret_cast<GameGetVersionFunc>(GetProcAddress(newModule, "GameGetVersion"));
    auto pUpdate  = reinterpret_cast<GameUpdateFunc>(GetProcAddress(newModule, "GameUpdate"));
    if (!pVersion || !pUpdate) {
        SM_ERROR("GameLibrary: missing required exports");
        FreeLibrary(newModule);
        fs::remove(copyPath, ec);
        return false;
    }

    // 5. Validate version.
    const uint32_t v = pVersion();
    if (v != GAME_API_VERSION) {
        SM_ERROR("GameLibrary: API version mismatch (editor=%u, dll=%u). Rebuild both targets.",
                 GAME_API_VERSION, v);
        FreeLibrary(newModule);
        fs::remove(copyPath, ec);
        return false;
    }

    // 6. Resolve optional symbols (nullptr-tolerant).
    auto pResize = reinterpret_cast<GameResizeFunc>(GetProcAddress(newModule, "GameResize"));
    auto pExit   = reinterpret_cast<GameExitFunc>(GetProcAddress(newModule, "GameExit"));

    // 7. Tear down OLD module: call GameExit, FreeLibrary, delete copy.
    if (m_Module) {
        if (m_pGameExit) m_pGameExit(state);
        FreeLibrary(m_Module);
        fs::remove(m_LoadedDllPath, ec);
        SM_TRACE("GameLibrary: unloaded previous module '%s'", m_LoadedDllPath.c_str());
    }

    // 8. Install new pointers.
    m_Module          = newModule;
    m_LoadedDllPath   = copyPath;
    m_pGameUpdate     = pUpdate;
    m_pGameResize     = pResize;
    m_pGameExit       = pExit;
    m_pGameGetVersion = pVersion;

    SM_TRACE("GameLibrary: loaded '%s' (API v%u)", copyPath.c_str(), v);
    return true;
}
```

Failure mode summary:

| Failure | Effect |
|---|---|
| Source DLL absent | `copy_file` fails → log warn, return false. Watcher retries on next event. |
| Source DLL being written | Same. Or `LoadLibraryA` fails → cleanup, return false. |
| Missing required export | Log error, free new module, delete copy, return false. Previous module stays. |
| Version mismatch | Same. Previous module stays. |
| `GameExit` on outgoing module throws | Not isolated — process dies. Keep `GameExit` benign. (Optional future: SEH guard.) |

---

## File watcher + reload coordination

Watcher runs on `filewatch::FileWatch`'s internal background thread (already vendored in `src/common/include/FileWatch.h`). Signals `GameThread` via a single `std::atomic<bool>`. GameThread consumes at tick boundary. No mutex.

### Watcher startup

```cpp
// In GameThread::RunLoop, after initial m_GameLib.LoadOrReload(...)
const std::string watchDir = "bin/Debug";  // VS_DEBUGGER_WORKING_DIRECTORY = RUNTIME_DIR
m_GameDllWatcher = std::make_unique<filewatch::FileWatch<std::string>>(
    watchDir,
    std::regex(R"(^Game\.dll$)"),  // exact match, ignore Game_load_*.dll copies
    [this](const std::string& file, const filewatch::Event evt) {
        if (evt == filewatch::Event::modified || evt == filewatch::Event::added) {
            m_ReloadPending.store(true, std::memory_order_release);
        }
    });
```

### GameThread main loop

```cpp
while (Running()) {
    // 1. Drain reload flag BEFORE input/commands/game logic.
    if (m_ReloadPending.exchange(false, std::memory_order_acquire)) {
        m_GameLib.LoadOrReload("bin/Debug/Game.dll", &gameState);
    }

    // 2. Read GameThreadSettings, drain ECSCommandRing (existing).
    // 3. Drain completed model loads, drain RGCommandRing (existing).
    // 4. Run game tick:
    if (m_GameLib.IsValid()) {
        m_GameLib.Update(&gameState);
    }
    // 5. Update plugins, publish snapshot, frame pacing (existing).
}
```

### Why a single atomic bool is sufficient

- Watcher thread writes `true` via `store(release)`.
- GameThread reads via `exchange(false, acquire)`.
- Release/acquire pair guarantees memory visibility of any prior watcher writes (there are none beyond the flag itself).
- Multiple watcher events between two GameThread reads collapse into one reload — that's the intended debounce.

### Reload ordering

Reload runs **before** ECS-command drain and game logic. The new DLL sees commands queued since the previous tick, and the snapshot published this tick reflects new code's behavior. Lowest visible-latency reload. Crashes attributable to the reload surface inside the tick that just reloaded.

### Initial load

```cpp
if (!m_GameLib.LoadOrReload("bin/Debug/Game.dll", &gameState)) {
    SM_ERROR("GameThread: initial Game.dll load failed. Editor will run without game logic until Game.dll becomes loadable.");
    // Don't abort. Watcher installed; user fixes build → next event triggers retry.
}
```

Editor remains usable through a failed initial load.

### Watcher teardown

`m_GameDllWatcher.reset()` (or unique_ptr going out of scope at `RunLoop` end) joins the watcher's threads cleanly. No explicit coordination needed in `GameThread::Stop()`.

### Edge cases

| Scenario | Behavior |
|---|---|
| User deletes `Game.dll` | Watcher fires `removed` (filtered out). Game continues with last-loaded module. |
| Build deletes then writes `Game.dll` | `removed` ignored; `added` triggers reload. |
| User saves source without building | No FS change to `Game.dll`. No reload — desired. |
| Antivirus locks during load | `copy_file` / `LoadLibraryA` fails → log → keep previous. Next event retries. |
| GameThread stopping during reload | Reload completes synchronously. Loop exits next iter. |

---

## State migration (file-static → GameState)

Hot-reload resets all file-static globals in `Game.cpp` (new module's BSS is fresh). Cross-tick state must move into editor-owned `GameState`.

### Audit of current file-static state in Game.cpp

| Variable | Action |
|---|---|
| `g_GameState` | Keep file-static — just caches incoming pointer; re-bound on first GameUpdate call. |
| `gKeysDown[KEY_LAST+1]` | Move to GameState. Held-key state must persist. |
| `gKeysPressedThisFrame[KEY_LAST+1]` | Keep file-static — per-tick only. |
| `gMouseWheel` | Keep file-static — consumed per tick. |
| `g_MouseAimEnabled` | Move to GameState. |
| `g_MouseX`, `g_MouseY` | Move to GameState. |
| `textEntityId` | Move to GameState — EntityId into editor ECS. Critical. |
| `g_DayNightCycleSeconds` | Move to GameState. |
| `g_DirectionalLightEntity` | Move to GameState — EntityId. Critical. |

### Revised GameState

```cpp
struct GameState {
    // ----- existing -----
    GameStateId   StateId = GameStateId::Uninitialized;
    double        GameTime = 0.0;
    double        DeltaTime = 0.0;
    double        TargetTPS = 60.0;
    double        ActualTPS = 0.0;
    SpscRing<InputEvent, ApplicationContext::InputRingSize>* PlatformInput = nullptr;
    void*         GameOutputHandle = nullptr;
    const ApplicationSettings* Settings = nullptr;
    bool          QuitRequested = false;
    PerspectiveCamera3D  GameCamera{};
    OrthographicCamera2D UICamera{};
    ECS           World{};

    // ----- new: persistent input state -----
    bool   KeysDown[KEY_LAST + 1] = {};
    bool   MouseAimEnabled = false;
    double MouseX = 0.0;
    double MouseY = 0.0;

    // ----- new: persistent game-level handles & settings -----
    EntityId TextEntity             = INVALID_ENTITY;
    EntityId DirectionalLightEntity = INVALID_ENTITY;
    float    DayNightCycleSeconds   = 10.0f;
    bool     WorldLoaded            = false;
};
```

Adding fields → bumps `GAME_API_VERSION`. Editor + game must rebuild in sync. Same constraint as before.

### Idempotent init

Entity creation must check `INVALID_ENTITY` sentinel so reload doesn't double-create:

```cpp
if (g_GameState->DirectionalLightEntity == INVALID_ENTITY) {
    g_GameState->DirectionalLightEntity = g_GameState->World.CreateEntity();
    g_GameState->World.AddComponent(g_GameState->DirectionalLightEntity, LightningComponent{...});
}
```

### World load relocation

The single editor reach-in (`game.cpp:72`):

```cpp
// Before
if (WorldManager::LoadWorldSnapshot(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH, &g_GameState->World)) { ... }
```

Moves to editor. `GameThread::RunLoop` calls `WorldManager::LoadWorldSnapshot(DEFAULT_WORLD_SNAPSHOT_PATH, &gameState.World)` once during initial setup, BEFORE the first call to `m_GameLib.Update(&gameState)`. `GameState::WorldLoaded` guards re-load on reload. `game.cpp` drops the `#include "../../editor/src/utilities/WorldManager.h"` line entirely.

After this, `game.cpp` has zero references to editor headers. Clean DLL boundary.

### Game.cpp reference rewrites

Every reference to a migrated file-static becomes `g_GameState->`. `IsKeyDown(k)` updates to use `g_GameState->KeysDown[k]`. `IsKeyPressedThisFrame(k)` continues using per-tick file-static.

Estimate: ~20 line edits in game.cpp. Mechanical.

---

## `ecs.dll` build setup

### CMake target

```cmake
# src/ecs/CMakeLists.txt

add_library(ecs SHARED
    src/ecs.cpp
)

target_include_directories(ecs PUBLIC ../common/include)

target_link_libraries(ecs PUBLIC
    CommonHeaders
    glm::glm
)

target_compile_definitions(ecs PRIVATE
    ECS_EXPORTS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    ${GLM_DEFINES}
)

set_target_properties(ecs PROPERTIES
    OUTPUT_NAME ecs
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Libraries
)

if(MSVC)
    set_property(TARGET ecs PROPERTY MSVC_DEBUG_INFORMATION_FORMAT
                 $<$<CONFIG:Debug>:Embedded>)
endif()
```

Root `CMakeLists.txt`:

```cmake
add_subdirectory(third_party)
add_subdirectory(src/common)
add_subdirectory(src/ecs)        # NEW — before game and editor
add_subdirectory(src/game)
add_subdirectory(src/overlay)
add_subdirectory(src/runtime)
add_subdirectory(src/editor)
add_subdirectory(tests)
```

CRT linkage locked for all targets:

```cmake
# Root CMakeLists.txt, near top
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    CACHE STRING "" FORCE)
```

### `ECS_API` macro

```cpp
// ECS.h, near top
#ifndef ECS_API
  #ifdef _WIN32
    #ifdef ECS_EXPORTS
      #define ECS_API __declspec(dllexport)
    #else
      #define ECS_API __declspec(dllimport)
    #endif
  #else
    #define ECS_API
  #endif
#endif
```

### X-macro: registered component types

Single source of truth for which `ComponentArray<T>` instantiations live in `ecs.dll`:

```cpp
// ECS.h
#define ECS_FOR_EACH_REGISTERED_COMPONENT(X) \
    X(TransformComponent) \
    X(MeshComponent) \
    X(MaterialComponent) \
    X(TextComponent) \
    X(LightningComponent) \
    X(ParentComponent) \
    X(ChildComponent)
```

### Inline vs out-of-line decision

Header-inline (no shared_ptr created; safe to inline into caller's TU):

- `ComponentArray<T>::Add`, `Remove`, `Has`, `Size`, `Get`, `GetComponents`, `GetEntity`
- `EntityStore::*`
- `ECS::CreateEntity`, `IsValidEntity`, `GetEntityCount`, `GetActiveEntities`
- `ECS::HasComponent<T>` const, `GetComponent<T>` const
- `ECS::HasComponents<T...>`, `GetComponents<T...>` const, `View<T...>`
- `ECS::Modify<T, F>` — thin wrapper around exported `MutateArray<T>` + user lambda

Out-of-line in `ecs.cpp` (creates `shared_ptr<ComponentArray<T>>`, must live in ecs.dll):

- `ComponentArray<T>::Clone()` — virtual; vtable lives in instantiating TU.
- `ComponentStore::MutateArray<T>`, `AddComponent<T>`, `RemoveComponent<T>`, `HasComponent<T>` const, `GetComponent<T>` const, `GetArray<T>` const
- `ComponentStore::RemoveAllComponents`, `CopyArraysFrom`, `ClearDirty`
- `ECS::AddComponent<T>`, `RemoveComponent<T>`, `MutateArray<T>`, `GetArray<T>` const (per-T)
- `ECS::DestroyEntity`, `CreateSnapshot`, `Clear`

### `ECS.h` extern-template declarations

```cpp
// After class ComponentArray<T> definition
#define ECS_EXTERN_TEMPLATE_DECL(T) extern template class ECS_API ComponentArray<T>;
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_EXTERN_TEMPLATE_DECL)
#undef ECS_EXTERN_TEMPLATE_DECL

// After ComponentStore class declaration
#define ECS_EXTERN_TEMPLATE_METHOD_DECLS(T) \
    extern template ECS_API ComponentArray<T>& ComponentStore::MutateArray<T>(); \
    extern template ECS_API const ComponentArray<T>* ComponentStore::GetArray<T>() const; \
    extern template ECS_API void ComponentStore::AddComponent<T>(EntityId, T); \
    extern template ECS_API void ComponentStore::RemoveComponent<T>(EntityId); \
    extern template ECS_API bool ComponentStore::HasComponent<T>(EntityId) const; \
    extern template ECS_API const T* ComponentStore::GetComponent<T>(EntityId) const;
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_EXTERN_TEMPLATE_METHOD_DECLS)
#undef ECS_EXTERN_TEMPLATE_METHOD_DECLS

// After ECS class declaration
#define ECS_EXTERN_TEMPLATE_ECS_METHODS(T) \
    extern template ECS_API void ECS::AddComponent<T>(EntityId, T); \
    extern template ECS_API void ECS::RemoveComponent<T>(EntityId); \
    extern template ECS_API bool ECS::HasComponent<T>(EntityId) const; \
    extern template ECS_API const T* ECS::GetComponent<T>(EntityId) const; \
    extern template ECS_API const ComponentArray<T>* ECS::GetArray<T>() const; \
    extern template ECS_API ComponentArray<T>& ECS::MutateArray<T>();
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_EXTERN_TEMPLATE_ECS_METHODS)
#undef ECS_EXTERN_TEMPLATE_ECS_METHODS
```

### `src/ecs/src/ecs.cpp`

```cpp
#include "ECS.h"

// 1) Class template instantiation per registered T (emits vtable + Clone body).
#define ECS_INSTANTIATE_CLASS(T) template class ComponentArray<T>;
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_INSTANTIATE_CLASS)
#undef ECS_INSTANTIATE_CLASS

// 2) Non-templated definitions moved from header:
//    ComponentStore::RemoveAllComponents, CopyArraysFrom, ClearDirty
//    ECS::DestroyEntity, CreateSnapshot, Clear

// 3) Templated method definitions moved from header (one body each).
//    e.g. ComponentStore::MutateArray<T>, AddComponent<T>, RemoveComponent<T>, etc.
//         ECS::AddComponent<T>, MutateArray<T>, GetArray<T>, etc.
//    ComponentArray<T>::Clone() is also defined here (was in header before).

// 4) Explicit instantiation per registered T.
#define ECS_INSTANTIATE_METHODS(T) \
    template ComponentArray<T>& ComponentStore::MutateArray<T>(); \
    template const ComponentArray<T>* ComponentStore::GetArray<T>() const; \
    template void ComponentStore::AddComponent<T>(EntityId, T); \
    template void ComponentStore::RemoveComponent<T>(EntityId); \
    template bool ComponentStore::HasComponent<T>(EntityId) const; \
    template const T* ComponentStore::GetComponent<T>(EntityId) const; \
    template void ECS::AddComponent<T>(EntityId, T); \
    template void ECS::RemoveComponent<T>(EntityId); \
    template bool ECS::HasComponent<T>(EntityId) const; \
    template const T* ECS::GetComponent<T>(EntityId) const; \
    template const ComponentArray<T>* ECS::GetArray<T>() const; \
    template ComponentArray<T>& ECS::MutateArray<T>();
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_INSTANTIATE_METHODS)
#undef ECS_INSTANTIATE_METHODS
```

### Adding a new component type

1. Declare struct in `ECS.h`.
2. Add `X(NewType)` line to `ECS_FOR_EACH_REGISTERED_COMPONENT`.
3. Add handling to `ECSCommandProcessor::ApplyComponentCommand` and `RemoveComponentByType` in `ApplicationContext.h` (pre-existing footgun).
4. Optional: ImGui editor UI in `ImGuiRenderer.cpp`.

Rebuild `ecs` + `editor` + `game`.

### `CommonHeaders` and `runtime`

`CommonHeaders` stays an INTERFACE library, still provides all headers (including `ECS.h`). Does not link `ecs`. Editor / game / test_ecs link both `CommonHeaders` (headers) and `ecs` (symbols).

Legacy `runtime` target is unchanged.

---

## `game.dll` build setup

```cmake
# src/game/CMakeLists.txt

add_library(game SHARED                # was STATIC
    src/Game.cpp
)

target_include_directories(game PUBLIC include)

target_link_libraries(game PRIVATE
    CommonHeaders
    ecs
    glm::glm
)

target_compile_definitions(game PRIVATE
    GAME_EXPORTS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    ${GLM_DEFINES}
)

set_target_properties(game PROPERTIES
    OUTPUT_NAME Game
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Libraries
)

if(MSVC)
    set_property(TARGET game PROPERTY MSVC_DEBUG_INFORMATION_FORMAT
                 $<$<CONFIG:Debug>:Embedded>)
endif()

# Clean stale reload copies before each build.
add_custom_command(TARGET game PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f
            "${RUNTIME_DIR}/Game_load_*.dll"
            "${RUNTIME_DIR}/Game_load_*.pdb"
    VERBATIM
    COMMENT "Wiping stale Game_load_*.dll copies before rebuild"
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(game PRIVATE
        -Wno-switch -Wno-writable-strings -Wno-sign-compare
        -Wno-deprecated-declarations -Wno-format-security -Wmissing-braces)
endif()

add_library(GameHeaders INTERFACE)
target_include_directories(GameHeaders INTERFACE include)
target_link_libraries(GameHeaders INTERFACE glm::glm CommonHeaders)
```

### Editor CMake changes

```cmake
# src/editor/CMakeLists.txt

# Remove `game` from target_link_libraries(editor PRIVATE ...).
# Add `GameHeaders` (Game.h types) and `ecs` (ECS symbols).

target_link_libraries(editor PRIVATE
    GameHeaders                       # for Game.h type definitions only
    ecs                               # ECS via ecs.dll
    # ... existing libs (NVRHI, ImGui, ImGuizmo, assimp, freetype, nethost, etc.)
)

add_dependencies(editor game)         # ensures Game.dll is built when editor builds
```

### Why `/Z7` (embedded debug info)

Without it: MSVC produces `Game.pdb` next to `Game.dll`. While the editor is loaded with `Game-<timestamp>.dll`, the `Game.pdb` is held by the debugger. The next link attempts to overwrite `Game.pdb` and fails with `LNK1201`.

With `/Z7`: debug info is embedded in `.obj` files and merged into the DLL's `.debug` section. No `Game.pdb` exists. Linker has nothing to lock.

### Why `PRE_BUILD` for cleanup, not `POST_BUILD`

PRE_BUILD runs before the linker writes the new `Game.dll`. The currently-loaded `Game_load_<old>.dll` may still be held by editor (Windows refuses delete → command silently leaves it; `rm -f` ignores failures). Between this and `LoadOrReload`'s own cleanup of the previous timestamped copy, at most one or two stale files accumulate.

### Build flow user-perspective

```
1. Edit src/game/src/Game.cpp.
2. cmake --build --preset msvc-win64-vs2026-enterprise --target game
3. CMake invokes PRE_BUILD cleanup, builds Game.dll into bin/Debug/.
4. Editor's filewatch::FileWatch detects Game.dll modified.
5. m_ReloadPending set → next GameThread tick performs LoadOrReload.
6. New code is live. Editor never restarted.
```

Iteration: 2–5 seconds depending on incremental compile speed.

### Header-change rule

If `Game.h` or `ECS.h` changes (struct layout / API signature):
- Rebuild **all** targets (`ecs`, `editor`, `game`).
- Restart editor (existing `editor.exe` was linked against old `ecs.dll` and old `Game.h` layouts).
- `GAME_API_VERSION` bump enforces this — mismatch rejects reload.

Pragmatic: `.cpp`-only change → hot-reload. `.h` change → restart.

### Naming convention

`Game_load_<microsec_timestamp>_<counter>.dll`. The counter handles same-microsecond collisions during build storms.

---

## Error handling and invariants

### Failure catalogue

| Source | Editor behavior | Remediation |
|---|---|---|
| Compile error in Game.cpp | None (no FS change) | Fix, rebuild |
| Link error (partial DLL) | `LoadLibraryA` rejects → keep previous | Fix, rebuild |
| Missing required export in new DLL | Log error, keep previous | Fix Game.cpp, rebuild |
| `GAME_API_VERSION` mismatch | Log error, keep previous | Rebuild both targets |
| File-locked during copy | Log warn, retry next event | Wait |
| `GameUpdate` crashes | Process dies | Fix, restart, rebuild |
| `GameExit` crashes during reload | Process dies | Keep GameExit benign |
| Allocation failure in ecs.dll | Process dies (OOM) | Out of scope |
| Watcher thread crashes | Hot-reload stops; editor keeps running | Restart |
| Game.dll deleted | Filter ignores `removed`; game continues with last-loaded module | None |

### Single-writer invariant under reload

`m_GameLib` is owned only by GameThread. Watcher only sets `m_ReloadPending` — never touches `GameLibrary`. No mutex needed.

`AssertOwnerThread` (debug-only) in `ComponentStore` verifies all ECS mutations come from the thread that constructed it. Game.dll's `GameUpdate` is invoked synchronously by GameThread → all ECS calls from game.dll are on GameThread → asserts hold.

### Reload during model-load worker

Background worker pushes results into `m_CompletedJobs`. GameThread drains them at tick start, AFTER reload. New DLL's drain code consumes the queue normally. Entities referenced in pending results live in editor-owned ECS — survive reload.

### Reload during ImGui inspector edits

ImGui pushes onto `ECSCommandRing`. GameThread drains AFTER reload. New DLL processes the commands via the same `ECSCommandProcessor` (which lives in `ApplicationContext.h` and is identical across editor and game).

### Reload during render frame

RenderThread is fully decoupled. Reads `LatestWorldSnapshot` (shared_ptr) + Seqlock SimulationSnapshot. Cloned `ComponentArray<T>` instances were allocated via ecs.dll's exported `MutateArray<T>` → deleters live in ecs.dll → safe across game.dll unload.

### Debugger UX

Visual Studio / RemedyBG attached to `editor.exe`. Symbols for `Game.dll` auto-load on `LoadLibrary`. With `/Z7`, embedded symbols resolve immediately. Breakpoints in `Game.cpp` survive reload as long as the source line content matches.

---

## Testing

### Unit-style: `test_ecs.exe` rebuilt against ecs.dll

Tests T01–T12 (~19 cases) from prior work continue to verify ECS COW invariants. After this change:

- `test_ecs` links `ecs.dll` instead of header-only ECS.
- Tests run against the **shipped** binary — same code path the editor uses.
- T12 (concurrent smoke) continues to verify snapshot isolation. Implicitly verifies the ecs.dll boundary by exercising the same MutateArray / Clone path the editor uses.

No new ECS tests for hot-reload itself.

### Manual smoke for hot-reload

```
1. Start editor.
2. Move directional light's color via ImGui inspector — verify visible change.
3. Edit Game.cpp: change a constant the per-tick code path reads
   (e.g., DayNightCycleSeconds derived value).
4. cmake --build --preset msvc-win64-vs2026-enterprise --target game
5. Watch editor console for "GameLibrary: loaded 'Game_load_<ts>.dll' (API v1)".
6. Verify game still runs without restart.
7. Verify entity IDs preserved: TextEntity and DirectionalLightEntity still
   in inspector with same IDs.
8. Verify input state preserved: hold W before reload → still moving forward after reload.
9. Edit Game.cpp to add a deliberate idempotency violation. Rebuild.
   Verify: no double-spawn of directional light (sentinel guard works).
10. Edit Game.h adding a field to GameState. Rebuild ALL targets.
    Verify: editor must be restarted. Without restart, GAME_API_VERSION
    mismatch rejects the reload and logs the expected error.
```

### Documentation updates

- `CLAUDE.md`: add "Hot-reloading game.dll" section with user-facing build flow + header-change rebuild rule.
- `CMakeLists.txt` comments: explain `/Z7` and PRE_BUILD cleanup choices.
- `Game.h`: doc comment near `GAME_API_VERSION` describing when to bump.

---

## Rollback

The change is mostly additive. Pieces specific to hot-reload (the rollback target):

- `src/editor/src/threading/GameLibrary.{h,cpp}`
- `m_GameDllWatcher` and `m_ReloadPending` fields in `GameThread`
- `target_link_libraries(editor)` `ecs`+`GameHeaders` swap-back to `game` direct link
- `game` CMakeLists STATIC ↔ SHARED toggle and `/Z7` + PRE_BUILD cleanup

Pieces that stay valuable even if hot-reload is reverted:

- `ecs.dll` as a separate target (cleaner architecture, gives test_ecs the shipped binary)
- `GameState` field migration (§"State migration"; cleaner game.cpp regardless)
- X-macro for component type registration (single source of truth)

---

## Out of scope

- SEH-guarded `GameUpdate` invocation
- `GameSerialize` / `GameDeserialize` exports for cross-version state migration
- Hot-reloading `ecs.dll` itself
- Linux / macOS file watchers + `.so` / `.dylib` equivalents
- Multiple game DLL variants loaded simultaneously
- ImGui-driven build invocation ("Rebuild & Reload" button)
- `Doxyfile` / HTML doc generation
- `runtime` target migration to ecs.dll
