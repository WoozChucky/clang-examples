# Game Library Hot-Reload Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Re-enable hot-reload of `Game.dll` for the editor under the 3-thread model, with ECS code extracted into a new `ecs.dll` to keep cross-DLL shared_ptr deleters always-loaded.

**Architecture:** Single-writer/snapshot-reader model preserved. ECS code moves out of header-only and into `ecs.dll`, exported via `ECS_API`. `game` switches from STATIC to SHARED. New `GameLibrary` class wraps the dynamic load: copies `Game.dll` to a timestamped path, validates `GAME_API_VERSION`, swaps function pointers between ticks. A `filewatch::FileWatch` on the runtime directory sets `std::atomic<bool> m_ReloadPending` from a background thread; `GameThread` drains it at the top of each tick. Game.cpp's file-static globals migrate into editor-owned `GameState` so reload preserves them.

**Tech Stack:** C++23, CMake (presets), MSVC 14.5x via Visual Studio 2026 generator, GLM, ecs.dll (new SHARED), Game.dll (SHARED), Win32 LoadLibrary, filewatch::FileWatch.

**Spec:** `docs/superpowers/specs/2026-05-19-game-hot-reload-design.md` — read first.

---

## File Structure

**Created:**
- `src/ecs/CMakeLists.txt` — defines `ecs` SHARED target.
- `src/ecs/src/ecs.cpp` — explicit template instantiations + non-templated method definitions.
- `src/editor/src/threading/GameLibrary.h` — RAII handle around HMODULE + symbol pointers.
- `src/editor/src/threading/GameLibrary.cpp` — LoadOrReload + Unload implementation.

**Modified:**
- `CMakeLists.txt` (root) — `CMAKE_MSVC_RUNTIME_LIBRARY` lockdown; new `add_subdirectory(src/ecs)`.
- `src/common/include/ECS.h` — `ECS_API` macro, X-macro registration, `extern template` declarations, class export annotations, body relocations.
- `src/game/CMakeLists.txt` — STATIC → SHARED, link `ecs`, `/Z7`, PRE_BUILD cleanup.
- `src/game/include/Game.h` — `GAME_API_VERSION` macro, new `GameState` fields.
- `src/game/src/Game.cpp` — `GameGetVersion()` returns `GAME_API_VERSION`; drop `WorldManager.h` include; replace file-static globals with `g_GameState->` access; idempotent entity-init guards.
- `src/editor/CMakeLists.txt` — drop direct `game` link; add `ecs` link; new `GameLibrary.{h,cpp}` source entries.
- `src/editor/src/threading/GameThread.h` — `GameLibrary m_GameLib`, `std::atomic<bool> m_ReloadPending`, `std::unique_ptr<filewatch::FileWatch<std::string>> m_GameDllWatcher`.
- `src/editor/src/threading/GameThread.cpp` — initial LoadOrReload, world-load relocation, watcher setup, reload-flag drain at tick start, `m_GameLib.Update(&gameState)` instead of direct call.
- `tests/CMakeLists.txt` — link `ecs` target.

**Build/run commands (used throughout):**

- Configure: `cmake --preset msvc-win64-vs2026-enterprise`
- Build all: `cmake --build --preset msvc-win64-vs2026-enterprise`
- Build editor only: `cmake --build --preset msvc-win64-vs2026-enterprise --target editor`
- Build game only: `cmake --build --preset msvc-win64-vs2026-enterprise --target game`
- Build ecs only: `cmake --build --preset msvc-win64-vs2026-enterprise --target ecs`
- Build test_ecs: `cmake --build --preset msvc-win64-vs2026-enterprise --target test_ecs`
- Run tests: `./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_ecs.exe`
- Run editor: `./out/build/msvc-win64-vs2026-enterprise/bin/Debug/editor.exe`

---

## Task 1: Lock CRT runtime library

Single CRT (`/MDd` in Debug) across all targets ensures consistent `std::vector` / `std::shared_ptr` / `std::unordered_map` layout, which is required for safe cross-DLL container operations. CMake's default already chooses `MultiThreadedDebugDLL` in Debug, but lock it explicitly to prevent future drift.

**Files:**
- Modify: `CMakeLists.txt` (root)

- [ ] **Step 1: Add CMAKE_MSVC_RUNTIME_LIBRARY lockdown to root CMakeLists.txt**

In `CMakeLists.txt`, add after the existing `project(clang_examples LANGUAGES CXX)` line:

```cmake
# Lock all targets to the same MSVC runtime library so STL containers cross
# DLL boundaries safely (editor.exe, ecs.dll, game.dll, test_ecs.exe).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    CACHE STRING "" FORCE)
```

- [ ] **Step 2: Re-configure to ensure the cache picks up the override**

Run:
```
cmake --preset msvc-win64-vs2026-enterprise
```

Expected: configuration succeeds. No new errors.

- [ ] **Step 3: Verify all existing targets still build cleanly**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
cmake --build --preset msvc-win64-vs2026-enterprise --target test_ecs
```

Expected: both targets build. test_ecs runs and prints `All ECS tests passed.`

- [ ] **Step 4: Commit**

```
git add CMakeLists.txt
git commit -m "Lock CMAKE_MSVC_RUNTIME_LIBRARY to MultiThreadedDebugDLL"
```

---

## Task 2: Create empty `ecs` SHARED target and wire into root build

Add `src/ecs/CMakeLists.txt` and `src/ecs/src/ecs.cpp` (currently empty content) so the target exists and builds, before any actual extraction work. Validates CMake plumbing in isolation.

**Files:**
- Create: `src/ecs/CMakeLists.txt`
- Create: `src/ecs/src/ecs.cpp`
- Modify: `CMakeLists.txt` (root)

- [ ] **Step 1: Create the empty ecs source file**

Create `src/ecs/src/ecs.cpp` with content:

```cpp
// ECS code lives here. Filled in over subsequent tasks (explicit template
// instantiations + out-of-line method definitions).
#include "ECS.h"
```

- [ ] **Step 2: Create the ecs target CMakeLists**

Create `src/ecs/CMakeLists.txt`:

```cmake
add_library(ecs SHARED
    src/ecs.cpp
)

target_include_directories(ecs PUBLIC ../common/include)

target_link_libraries(ecs PUBLIC
    CommonHeaders
    glm::glm
)

set(GLM_DEFINES "GLM_FORCE_DEPTH_ZERO_TO_ONE; GLM_FORCE_RIGHT_HANDED; GLM_ENABLE_EXPERIMENTAL;")

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

- [ ] **Step 3: Add ecs subdirectory to root CMakeLists.txt before game and editor**

In `CMakeLists.txt`, after `add_subdirectory(src/common)` and BEFORE `add_subdirectory(src/game)`, add:

```cmake
# ECS shared library (must be configured before game/editor link it)
add_subdirectory(src/ecs)
```

- [ ] **Step 4: Configure + build the new target**

Run:
```
cmake --preset msvc-win64-vs2026-enterprise
cmake --build --preset msvc-win64-vs2026-enterprise --target ecs
```

Expected: `ecs.dll` and `ecs.lib` appear at `out/build/msvc-win64-vs2026-enterprise/bin/Debug/`. The DLL contains no exports yet because `ECS.h` has no `ECS_API` annotations, but the target builds.

- [ ] **Step 5: Verify editor and test_ecs still build (no link change yet)**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
cmake --build --preset msvc-win64-vs2026-enterprise --target test_ecs
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_ecs.exe
```

Expected: both build; test_ecs prints `All ECS tests passed.`

- [ ] **Step 6: Commit**

```
git add src/ecs/CMakeLists.txt src/ecs/src/ecs.cpp CMakeLists.txt
git commit -m "Add empty ecs SHARED target wired into root build"
```

---

## Task 3: Add `ECS_API` macro and X-macro registration to `ECS.h`

Adds the toggle macro that switches between dllexport and dllimport plus the single-source-of-truth list of registered component types. No semantic change yet.

**Files:**
- Modify: `src/common/include/ECS.h`

- [ ] **Step 1: Add ECS_API macro near the top of ECS.h**

In `src/common/include/ECS.h`, right after the `#pragma once` and the include block, add:

```cpp
// Cross-DLL export annotation. Defined as dllexport in ecs.dll's TU,
// dllimport everywhere else. Allow override (defining ECS_API empty
// externally) for static testing scenarios.
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

- [ ] **Step 2: Add X-macro listing registered component types**

In `src/common/include/ECS.h`, AFTER the component struct definitions (TransformComponent, MeshComponent, MaterialComponent, TextComponent, LightningComponent, ParentComponent, ChildComponent) and BEFORE the `IComponentArray` class declaration, add:

```cpp
// X-macro: single source of truth for the set of component types that get
// explicit template instantiations in ecs.dll. Adding a new component type
// requires (1) declaring the struct above, (2) adding an X(NewType) line here,
// (3) registering in ECSCommandProcessor in ApplicationContext.h.
#define ECS_FOR_EACH_REGISTERED_COMPONENT(X) \
    X(TransformComponent) \
    X(MeshComponent) \
    X(MaterialComponent) \
    X(TextComponent) \
    X(LightningComponent) \
    X(ParentComponent) \
    X(ChildComponent)
```

- [ ] **Step 3: Verify build (no behavior change expected)**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
cmake --build --preset msvc-win64-vs2026-enterprise --target test_ecs
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_ecs.exe
```

Expected: both build; test_ecs prints `All ECS tests passed.`

- [ ] **Step 4: Commit**

```
git add src/common/include/ECS.h
git commit -m "Add ECS_API macro and X-macro registration to ECS.h"
```

---

## Task 4: Mark `ComponentArray<T>` for export + explicit instantiation in ecs.dll

`ComponentArray<T>` is the type whose `Clone()` virtual stores a function pointer in the vtable per instantiation. Forcing the instantiation into ecs.dll places the vtable (and deleter for shared_ptr<ComponentArray<T>>) into the always-loaded module.

**Files:**
- Modify: `src/common/include/ECS.h`
- Modify: `src/ecs/src/ecs.cpp`

- [ ] **Step 1: Annotate `ComponentArray<T>` with ECS_API**

In `src/common/include/ECS.h`, find the `template<typename T>` `class ComponentArray final : public IComponentArray { ... }` declaration. Change the class line to:

```cpp
template<typename T>
class ECS_API ComponentArray final : public IComponentArray {
```

- [ ] **Step 2: Add `extern template` declarations after the class body**

Right after the closing brace `};` of the `ComponentArray<T>` class definition (still inside `ECS.h`), add:

```cpp
// Declare that ComponentArray<T> is instantiated elsewhere (in ecs.dll's TU).
// Prevents per-TU local instantiation in editor.exe, game.dll, test_ecs.exe;
// they link against ecs.dll's exported copy.
#define ECS_EXTERN_TEMPLATE_DECL(T) extern template class ECS_API ComponentArray<T>;
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_EXTERN_TEMPLATE_DECL)
#undef ECS_EXTERN_TEMPLATE_DECL
```

- [ ] **Step 3: Add explicit instantiations to ecs.cpp**

In `src/ecs/src/ecs.cpp`, replace the file content with:

```cpp
// ECS code home. All template instantiations + dllexport definitions live here.
#include "ECS.h"

// Explicit class template instantiations — emits one full copy of
// ComponentArray<T> (methods, vtable, RTTI) per registered T into ecs.dll.
#define ECS_INSTANTIATE_CLASS(T) template class ComponentArray<T>;
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_INSTANTIATE_CLASS)
#undef ECS_INSTANTIATE_CLASS
```

- [ ] **Step 4: Build ecs target**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target ecs
```

Expected: build succeeds. `dumpbin /exports ecs.dll` (optional verification) shows symbols for `ComponentArray<TransformComponent>::Clone()` etc.

- [ ] **Step 5: Build editor and test_ecs (they still see header-only paths for ECS / ComponentStore but use ecs.dll for ComponentArray<T>)**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
cmake --build --preset msvc-win64-vs2026-enterprise --target test_ecs
```

Expected: both build. The compiler emits dllimport stubs for `ComponentArray<T>` methods; linker resolves them against ecs.lib. **Editor and test_ecs do NOT yet link ecs.lib** — so this step will FAIL with linker errors like `unresolved external symbol __imp_??...ComponentArray@U...`.

Treat the linker error as expected — proceed to Step 6 to wire the link.

- [ ] **Step 6: Wire editor, game, test_ecs to link `ecs`**

In `src/editor/CMakeLists.txt`, find the line:
```cmake
target_link_libraries(editor PRIVATE CommonHeaders GameHeaders freetype Tracy::TracyClient glm::glm glfw nvrhi nvrhi_d3d12 nvrhi_d3d11 d3dcompiler dxgi dxcompiler nvrhi_vk imgui ImGuizmo assimp::assimp nlohmann_json::nlohmann_json game)
```

Change to add `ecs`:
```cmake
target_link_libraries(editor PRIVATE CommonHeaders GameHeaders ecs freetype Tracy::TracyClient glm::glm glfw nvrhi nvrhi_d3d12 nvrhi_d3d11 d3dcompiler dxgi dxcompiler nvrhi_vk imgui ImGuizmo assimp::assimp nlohmann_json::nlohmann_json game)
```

In `src/game/CMakeLists.txt`, find:
```cmake
target_link_libraries(game PRIVATE glm::glm CommonHeaders)
```

Change to:
```cmake
target_link_libraries(game PRIVATE glm::glm CommonHeaders ecs)
```

In `tests/CMakeLists.txt`, find:
```cmake
target_link_libraries(test_ecs PRIVATE
    CommonHeaders
    glm::glm
)
```

Change to:
```cmake
target_link_libraries(test_ecs PRIVATE
    CommonHeaders
    glm::glm
    ecs
)
```

- [ ] **Step 7: Re-configure and rebuild all targets**

Run:
```
cmake --preset msvc-win64-vs2026-enterprise
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
cmake --build --preset msvc-win64-vs2026-enterprise --target test_ecs
```

Expected: both build cleanly. test_ecs's previously-failing linker errors are resolved now.

- [ ] **Step 8: Run test_ecs to verify behavior is unchanged**

Run:
```
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_ecs.exe
```

Expected: `All ECS tests passed.` Exit code 0.

- [ ] **Step 9: Commit**

```
git add src/common/include/ECS.h src/ecs/src/ecs.cpp src/editor/CMakeLists.txt src/game/CMakeLists.txt tests/CMakeLists.txt
git commit -m "Export ComponentArray<T> from ecs.dll via explicit instantiation"
```

---

## Task 5: Mark `ComponentStore` for export + move templated methods to ecs.cpp

`ComponentStore::MutateArray<T>` creates the cloned `shared_ptr<ComponentArray<T>>`. Its instantiation must live in ecs.dll so the deleter survives game.dll unload.

**Files:**
- Modify: `src/common/include/ECS.h`
- Modify: `src/ecs/src/ecs.cpp`

- [ ] **Step 1: Annotate `ComponentStore` class with ECS_API**

In `src/common/include/ECS.h`, find `class ComponentStore { ... }`. Change the class line to:

```cpp
class ECS_API ComponentStore {
```

- [ ] **Step 2: Move templated method bodies into ecs.cpp**

The current header-inline templated methods on `ComponentStore` are: `MutateArray<T>`, `GetArray<T>`, `AddComponent<T>`, `RemoveComponent<T>`, `HasComponent<T>`, `GetComponent<T>` (the const overload — non-const was removed in prior work).

In `src/common/include/ECS.h`, find each of these methods inside `class ComponentStore` and replace its full definition with just a declaration. Example:

Before:
```cpp
template<typename T>
ComponentArray<T>& MutateArray() {
    AssertOwnerThread();
    const auto typeIndex = std::type_index(typeid(T));
    auto& slot = m_ComponentArrays[typeIndex];
    if (!slot) slot = std::make_shared<ComponentArray<T>>();
    if (m_DirtyThisTick.insert(typeIndex).second) {
        slot = std::make_shared<ComponentArray<T>>(
                   static_cast<const ComponentArray<T>&>(*slot));
    }
    return static_cast<ComponentArray<T>&>(*slot);
}
```

After (declaration only):
```cpp
template<typename T>
ComponentArray<T>& MutateArray();
```

Apply the same transform (full body → declaration) to:
- `template<typename T> const ComponentArray<T>* GetArray() const;`
- `template<typename T> void AddComponent(EntityId entity, T component);`
- `template<typename T> void RemoveComponent(EntityId entity);`
- `template<typename T> bool HasComponent(EntityId entity) const;`
- `template<typename T> const T* GetComponent(EntityId entity) const;`

Leave `RegisterComponent<T>` inline if it exists; it's a helper called from MutateArray and lives in the same TU.

- [ ] **Step 3: Add extern template declarations for ComponentStore methods after the class body**

Immediately after the closing `};` of `class ComponentStore`, add:

```cpp
// Per-T extern template declarations for ComponentStore methods.
#define ECS_EXTERN_COMPONENT_STORE_METHODS(T) \
    extern template ECS_API ComponentArray<T>& ComponentStore::MutateArray<T>(); \
    extern template ECS_API const ComponentArray<T>* ComponentStore::GetArray<T>() const; \
    extern template ECS_API void ComponentStore::AddComponent<T>(EntityId, T); \
    extern template ECS_API void ComponentStore::RemoveComponent<T>(EntityId); \
    extern template ECS_API bool ComponentStore::HasComponent<T>(EntityId) const; \
    extern template ECS_API const T* ComponentStore::GetComponent<T>(EntityId) const;
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_EXTERN_COMPONENT_STORE_METHODS)
#undef ECS_EXTERN_COMPONENT_STORE_METHODS
```

- [ ] **Step 4: Add method definitions and explicit instantiations to ecs.cpp**

In `src/ecs/src/ecs.cpp`, AFTER the existing `ECS_INSTANTIATE_CLASS` block, add:

```cpp
// ----- ComponentStore templated method definitions -----

template<typename T>
ComponentArray<T>& ComponentStore::MutateArray() {
    AssertOwnerThread();
    const auto typeIndex = std::type_index(typeid(T));
    auto& slot = m_ComponentArrays[typeIndex];
    if (!slot) slot = std::make_shared<ComponentArray<T>>();
    if (m_DirtyThisTick.insert(typeIndex).second) {
        slot = std::make_shared<ComponentArray<T>>(
                   static_cast<const ComponentArray<T>&>(*slot));
    }
    return static_cast<ComponentArray<T>&>(*slot);
}

template<typename T>
const ComponentArray<T>* ComponentStore::GetArray() const {
    const auto typeIndex = std::type_index(typeid(T));
    const auto it = m_ComponentArrays.find(typeIndex);
    if (it == m_ComponentArrays.end()) return nullptr;
    return static_cast<const ComponentArray<T>*>(it->second.get());
}

template<typename T>
void ComponentStore::AddComponent(EntityId entity, T component) {
    MutateArray<T>().Add(entity, component);
}

template<typename T>
void ComponentStore::RemoveComponent(EntityId entity) {
    MutateArray<T>().Remove(entity);
}

template<typename T>
bool ComponentStore::HasComponent(EntityId entity) const {
    const auto* arr = GetArray<T>();
    return arr && arr->Has(entity);
}

template<typename T>
const T* ComponentStore::GetComponent(EntityId entity) const {
    const auto* arr = GetArray<T>();
    if (!arr) return nullptr;
    return arr->Get(entity);
}

// ----- Explicit instantiations of ComponentStore methods per registered T -----

#define ECS_INSTANTIATE_COMPONENT_STORE_METHODS(T) \
    template ComponentArray<T>& ComponentStore::MutateArray<T>(); \
    template const ComponentArray<T>* ComponentStore::GetArray<T>() const; \
    template void ComponentStore::AddComponent<T>(EntityId, T); \
    template void ComponentStore::RemoveComponent<T>(EntityId); \
    template bool ComponentStore::HasComponent<T>(EntityId) const; \
    template const T* ComponentStore::GetComponent<T>(EntityId) const;
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_INSTANTIATE_COMPONENT_STORE_METHODS)
#undef ECS_INSTANTIATE_COMPONENT_STORE_METHODS
```

If the existing `ComponentStore::HasComponent<T>` body in the header used a different shape (e.g. calling `GetComponentArray<T>()` instead of `GetArray<T>()`), match the existing shape verbatim when transcribing. The intent is byte-equivalent behavior, just relocated.

- [ ] **Step 5: Build ecs target**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target ecs
```

Expected: builds. ecs.dll now exports `ComponentStore::MutateArray<TransformComponent>` etc.

- [ ] **Step 6: Build editor and test_ecs, verify behavior unchanged**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
cmake --build --preset msvc-win64-vs2026-enterprise --target test_ecs
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_ecs.exe
```

Expected: both build; tests pass.

- [ ] **Step 7: Commit**

```
git add src/common/include/ECS.h src/ecs/src/ecs.cpp
git commit -m "Export ComponentStore templated methods from ecs.dll"
```

---

## Task 6: Move `ComponentStore` non-templated methods to ecs.cpp

`RemoveAllComponents` calls virtual `Clone()`. To ensure the virtual dispatches to ecs.dll's vtable, the calling code itself must live in ecs.dll. `CopyArraysFrom`, `ClearDirty`, `RegisterComponent<T>` (if templated keep in header) — same treatment.

**Files:**
- Modify: `src/common/include/ECS.h`
- Modify: `src/ecs/src/ecs.cpp`

- [ ] **Step 1: Replace `ComponentStore::RemoveAllComponents` body in header with declaration**

In `src/common/include/ECS.h`, find:

```cpp
void RemoveAllComponents(EntityId entity) {
    AssertOwnerThread();
    for (auto& [type, slot] : m_ComponentArrays) {
        if (!slot->Has(entity)) continue;
        if (m_DirtyThisTick.insert(type).second) {
            slot = slot->Clone();
        }
        slot->Remove(entity);
    }
}
```

Replace with declaration:
```cpp
void RemoveAllComponents(EntityId entity);
```

- [ ] **Step 2: Replace `CopyArraysFrom` and `ClearDirty` bodies with declarations**

In `src/common/include/ECS.h`, find:
```cpp
void CopyArraysFrom(const ComponentStore& other) { m_ComponentArrays = other.m_ComponentArrays; }
void ClearDirty() { AssertOwnerThread(); m_DirtyThisTick.clear(); }
```

Replace with:
```cpp
void CopyArraysFrom(const ComponentStore& other);
void ClearDirty();
```

- [ ] **Step 3: Add definitions to ecs.cpp**

In `src/ecs/src/ecs.cpp`, AFTER the ComponentStore templated method definitions, add:

```cpp
// ----- ComponentStore non-templated method definitions -----

void ComponentStore::RemoveAllComponents(EntityId entity) {
    AssertOwnerThread();
    for (auto& [type, slot] : m_ComponentArrays) {
        if (!slot->Has(entity)) continue;
        if (m_DirtyThisTick.insert(type).second) {
            slot = slot->Clone();
        }
        slot->Remove(entity);
    }
}

void ComponentStore::CopyArraysFrom(const ComponentStore& other) {
    m_ComponentArrays = other.m_ComponentArrays;
}

void ComponentStore::ClearDirty() {
    AssertOwnerThread();
    m_DirtyThisTick.clear();
}
```

Note: `AssertOwnerThread` is currently a private inline `#ifndef NDEBUG` helper inside ComponentStore. Since it's inline, the definition is in the class body and visible to ecs.cpp via the included header. No change needed.

- [ ] **Step 4: Build and verify**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target ecs
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
cmake --build --preset msvc-win64-vs2026-enterprise --target test_ecs
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_ecs.exe
```

Expected: all build; tests pass.

- [ ] **Step 5: Commit**

```
git add src/common/include/ECS.h src/ecs/src/ecs.cpp
git commit -m "Export ComponentStore non-templated methods from ecs.dll"
```

---

## Task 7: Mark `ECS` for export + move templated and non-templated methods to ecs.cpp

Final ECS class to migrate. Includes the special `Modify<T,F>` which stays inline as a header-only wrapper.

**Files:**
- Modify: `src/common/include/ECS.h`
- Modify: `src/ecs/src/ecs.cpp`

- [ ] **Step 1: Annotate `ECS` class with ECS_API**

In `src/common/include/ECS.h`, find `class ECS { ... }`. Change the class line to:

```cpp
class ECS_API ECS {
```

- [ ] **Step 2: Replace ECS templated method bodies with declarations (except Modify)**

In `src/common/include/ECS.h`, for each of:
- `template<typename T> void AddComponent(EntityId entity, T component);`
- `template<typename T> void RemoveComponent(EntityId entity);`
- `template<typename T> bool HasComponent(EntityId entity) const;`
- `template<typename T> const T* GetComponent(EntityId entity) const;`
- `template<typename T> const ComponentArray<T>* GetArray() const;`
- `template<typename T> ComponentArray<T>& MutateArray();`

Replace the inline body with just the declaration.

**Keep `Modify<T, F>` inline** — it's a header-only wrapper templated on the lambda type `F`, which can't be pre-instantiated. Its body remains:

```cpp
template<typename T, typename F>
void Modify(EntityId e, F&& fn) {
    const auto* readArr = m_ComponentStore.GetArray<T>();
    if (!readArr || !readArr->Has(e)) return;
    auto& writeArr = m_ComponentStore.MutateArray<T>();
    if (T* c = writeArr.Get(e)) {
        std::forward<F>(fn)(*c);
    }
}
```

**Keep `HasComponents<T...>`, `GetComponents<T...>`, `View<T...>` inline** — they fold over per-T calls that ARE exported, so they don't themselves need exporting.

- [ ] **Step 3: Replace ECS non-templated methods with declarations**

In `src/common/include/ECS.h`, replace the inline bodies of:
- `void DestroyEntity(EntityId entity);` — declaration only.
- `std::shared_ptr<const ECS> CreateSnapshot();` — declaration only.
- `void Clear();` — declaration only.

Keep these inline (no allocation, no shared_ptr creation):
- `EntityId CreateEntity()`
- `bool IsValidEntity(EntityId)` const
- `size_t GetEntityCount()` const
- `const std::vector<EntityId>& GetActiveEntities()` const

- [ ] **Step 4: Add extern template declarations for ECS methods**

After the closing `};` of `class ECS`, add:

```cpp
// Per-T extern template declarations for ECS methods.
#define ECS_EXTERN_ECS_METHODS(T) \
    extern template ECS_API void ECS::AddComponent<T>(EntityId, T); \
    extern template ECS_API void ECS::RemoveComponent<T>(EntityId); \
    extern template ECS_API bool ECS::HasComponent<T>(EntityId) const; \
    extern template ECS_API const T* ECS::GetComponent<T>(EntityId) const; \
    extern template ECS_API const ComponentArray<T>* ECS::GetArray<T>() const; \
    extern template ECS_API ComponentArray<T>& ECS::MutateArray<T>();
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_EXTERN_ECS_METHODS)
#undef ECS_EXTERN_ECS_METHODS
```

- [ ] **Step 5: Add ECS method definitions and explicit instantiations to ecs.cpp**

In `src/ecs/src/ecs.cpp`, AFTER the ComponentStore method definitions, add:

```cpp
// ----- ECS templated method definitions -----

template<typename T>
void ECS::AddComponent(EntityId entity, T component) {
    m_ComponentStore.AddComponent<T>(entity, component);
}

template<typename T>
void ECS::RemoveComponent(EntityId entity) {
    m_ComponentStore.RemoveComponent<T>(entity);
}

template<typename T>
bool ECS::HasComponent(EntityId entity) const {
    return m_ComponentStore.HasComponent<T>(entity);
}

template<typename T>
const T* ECS::GetComponent(EntityId entity) const {
    return m_ComponentStore.GetComponent<T>(entity);
}

template<typename T>
const ComponentArray<T>* ECS::GetArray() const {
    return m_ComponentStore.GetArray<T>();
}

template<typename T>
ComponentArray<T>& ECS::MutateArray() {
    return m_ComponentStore.MutateArray<T>();
}

// ----- ECS non-templated method definitions -----

void ECS::DestroyEntity(EntityId entity) {
    m_ComponentStore.RemoveAllComponents(entity);
    m_EntityStore.DestroyEntity(entity);
}

std::shared_ptr<const ECS> ECS::CreateSnapshot() {
    auto snap = std::make_shared<ECS>();
    snap->m_EntityStore = m_EntityStore;
    snap->m_ComponentStore.CopyArraysFrom(m_ComponentStore);
    m_ComponentStore.ClearDirty();
    return snap;
}

void ECS::Clear() {
    m_EntityStore.Clear();
    m_ComponentStore.Cleanup();
}

// ----- ECS templated method explicit instantiations per registered T -----

#define ECS_INSTANTIATE_ECS_METHODS(T) \
    template void ECS::AddComponent<T>(EntityId, T); \
    template void ECS::RemoveComponent<T>(EntityId); \
    template bool ECS::HasComponent<T>(EntityId) const; \
    template const T* ECS::GetComponent<T>(EntityId) const; \
    template const ComponentArray<T>* ECS::GetArray<T>() const; \
    template ComponentArray<T>& ECS::MutateArray<T>();
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_INSTANTIATE_ECS_METHODS)
#undef ECS_INSTANTIATE_ECS_METHODS
```

If `ECS::Clear()` body in the existing header is different (e.g., doesn't call `m_ComponentStore.Cleanup()`), match the existing body verbatim. Same intent: byte-equivalent behavior, relocated.

- [ ] **Step 6: Build and verify**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target ecs
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
cmake --build --preset msvc-win64-vs2026-enterprise --target test_ecs
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_ecs.exe
```

Expected: all build; `All ECS tests passed.`

- [ ] **Step 7: Commit**

```
git add src/common/include/ECS.h src/ecs/src/ecs.cpp
git commit -m "Export ECS class methods from ecs.dll"
```

---

## Task 8: Switch `game` from STATIC to SHARED + `/Z7` + PRE_BUILD cleanup

Game becomes a real DLL. Editor still links its import library (Game.lib) at this stage, so the runtime loader still resolves `GameUpdate` etc. at process start. Validates SHARED conversion in isolation from the hot-reload mechanics.

**Files:**
- Modify: `src/game/CMakeLists.txt`

- [ ] **Step 1: Switch `add_library(game ...)` to SHARED**

In `src/game/CMakeLists.txt`, find:

```cmake
add_library(game STATIC
    src/Game.cpp
)
```

Change to:

```cmake
add_library(game SHARED
    src/Game.cpp
)
```

- [ ] **Step 2: Add `/Z7` embedded debug info for MSVC**

In `src/game/CMakeLists.txt`, AFTER the `set_target_properties(game ...)` block, add:

```cmake
# Embed debug info in OBJ files instead of writing a separate Game.pdb.
# Required for hot-reload — avoids LNK1201 when the editor holds Game.dll
# while the linker tries to rewrite Game.pdb.
if(MSVC)
    set_property(TARGET game PROPERTY MSVC_DEBUG_INFORMATION_FORMAT
                 $<$<CONFIG:Debug>:Embedded>)
endif()
```

- [ ] **Step 3: Add PRE_BUILD cleanup for stale reload copies**

In `src/game/CMakeLists.txt`, AFTER the new `/Z7` block, add:

```cmake
# Wipe stale Game_load_*.dll / .pdb copies before each game rebuild.
# Files currently held by a running editor will refuse delete; rm -f tolerates that.
add_custom_command(TARGET game PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f
            "${RUNTIME_DIR}/Game_load_*.dll"
            "${RUNTIME_DIR}/Game_load_*.pdb"
    VERBATIM
    COMMENT "Wiping stale Game_load_*.dll copies before rebuild"
)
```

- [ ] **Step 4: Add GAME_EXPORTS define to game**

The existing `Game.h` uses `EXPORT_FN` macros that expand to `__declspec(dllexport)` when `_WIN32` is defined. No explicit `GAME_EXPORTS` is needed for those to work, but add it for clarity and future-proofing.

In `src/game/CMakeLists.txt`, find:

```cmake
target_compile_definitions(game PRIVATE ${GLM_DEFINES})
```

Change to:

```cmake
target_compile_definitions(game PRIVATE GAME_EXPORTS NOMINMAX WIN32_LEAN_AND_MEAN ${GLM_DEFINES})
```

- [ ] **Step 5: Re-configure and rebuild**

Run:
```
cmake --preset msvc-win64-vs2026-enterprise
cmake --build --preset msvc-win64-vs2026-enterprise --target game
```

Expected: `Game.dll` and `Game.lib` (import library) appear at `out/build/msvc-win64-vs2026-enterprise/bin/Debug/`. No `Game.pdb` is generated (because `/Z7`).

- [ ] **Step 6: Rebuild editor and verify it still launches**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/editor.exe
```

Expected: editor launches and operates normally. The editor process is linked against `Game.lib` (import library); Windows loader resolves the import to `Game.dll` at startup. Game still runs.

If editor.exe launch fails with "The application was unable to start (0xc000007b)" or "Game.dll not found", verify `Game.dll` exists in the same directory as `editor.exe`.

Close the editor (Alt+F4 or window close).

- [ ] **Step 7: Commit**

```
git add src/game/CMakeLists.txt
git commit -m "Switch game library from STATIC to SHARED with /Z7 and reload cleanup"
```

---

## Task 9: Add `GAME_API_VERSION` to `Game.h` and fix `GameGetVersion()`

Game.cpp's existing `GameGetVersion()` returns `0`. The hot-reload load step compares against `GAME_API_VERSION` — needs a real value.

**Files:**
- Modify: `src/game/include/Game.h`
- Modify: `src/game/src/Game.cpp`

- [ ] **Step 1: Add `GAME_API_VERSION` to `Game.h`**

In `src/game/include/Game.h`, AFTER the existing `EXPORT_FN` macro definitions and BEFORE the `enum class GameStateId` definition, add:

```cpp
// Bump every time GameState layout changes or any export signature changes.
// Editor compares against the compiled-in value at load time; mismatch rejects
// the reload and keeps the previous Game.dll active.
#define GAME_API_VERSION 1u
```

- [ ] **Step 2: Update `GameGetVersion()` to return `GAME_API_VERSION`**

In `src/game/src/Game.cpp`, find:

```cpp
uint32_t GameGetVersion() {
    SM_TRACE("[GAMEDLL] GameGetVersion");
    return 0;
}
```

Change to:

```cpp
uint32_t GameGetVersion() {
    return GAME_API_VERSION;
}
```

(Remove the `SM_TRACE` call — `GameGetVersion` may be invoked many times during hot-reload validation; the log noise is unwanted.)

- [ ] **Step 3: Build game and verify**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target game
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
```

Expected: both build cleanly.

- [ ] **Step 4: Commit**

```
git add src/game/include/Game.h src/game/src/Game.cpp
git commit -m "Add GAME_API_VERSION and return it from GameGetVersion()"
```

---

## Task 10: Migrate file-static globals from Game.cpp into GameState

Reload resets all DLL-internal statics. Cross-tick game state must move into editor-owned `GameState`.

**Files:**
- Modify: `src/game/include/Game.h`
- Modify: `src/game/src/Game.cpp`

- [ ] **Step 1: Extend `GameState` struct in `Game.h`**

In `src/game/include/Game.h`, find the existing `struct GameState { ... }` definition. After the existing `ECS World{};` field but before the closing brace, add:

```cpp

    // ----- Persistent input state (survives Game.dll reload) -----
    bool   KeysDown[KEY_LAST + 1] = {};
    bool   MouseAimEnabled = false;
    double MouseX = 0.0;
    double MouseY = 0.0;

    // ----- Persistent game-level entity handles & settings -----
    EntityId TextEntity             = INVALID_ENTITY;
    EntityId DirectionalLightEntity = INVALID_ENTITY;
    float    DayNightCycleSeconds   = 10.0f;
    bool     WorldLoaded            = false;
```

- [ ] **Step 2: Remove migrated file-static globals from `Game.cpp`**

In `src/game/src/Game.cpp`, find these file-static declarations near the top:

```cpp
static bool gKeysDown[KEY_LAST + 1] = {};
static bool g_MouseAimEnabled = false;
static double g_MouseX = 0.0, g_MouseY = 0.0;
static uint64_t textEntityId = 0;
static float g_DayNightCycleSeconds = 10.0f;
static EntityId g_DirectionalLightEntity = INVALID_ENTITY;
```

Delete these lines. **Keep** these file-statics — they're per-tick scratch, fine to reset on reload:

```cpp
static GameState* g_GameState = nullptr;
static bool gKeysPressedThisFrame[KEY_LAST + 1] = {};
static int32_t gMouseWheel = 0;
```

- [ ] **Step 3: Update all references in Game.cpp to use `g_GameState->*` instead**

Search-and-replace in `src/game/src/Game.cpp`:

| Find | Replace with |
|---|---|
| `gKeysDown[` | `g_GameState->KeysDown[` |
| `g_MouseAimEnabled` | `g_GameState->MouseAimEnabled` |
| `g_MouseX` | `g_GameState->MouseX` |
| `g_MouseY` | `g_GameState->MouseY` |
| `textEntityId` | `g_GameState->TextEntity` |
| `g_DayNightCycleSeconds` | `g_GameState->DayNightCycleSeconds` |
| `g_DirectionalLightEntity` | `g_GameState->DirectionalLightEntity` |

Use exact-match replacements. Some patterns appear in helper functions like `IsKeyDown(int key)` — update those too.

Note: `IsKeyDown` is defined as:
```cpp
inline bool IsKeyDown(int key) {
    if (key < 0 || key > KEY_LAST) return false;
    return gKeysDown[key];
}
```
After replace, it becomes:
```cpp
inline bool IsKeyDown(int key) {
    if (key < 0 || key > KEY_LAST) return false;
    return g_GameState->KeysDown[key];
}
```
This is correct only when `g_GameState` is non-null. The function is called from inside `GameUpdate` where `g_GameState` was already assigned. Existing call sites are safe.

- [ ] **Step 4: Add idempotent guards on entity creation**

In `src/game/src/Game.cpp`, find each place where entities are created and stored into `g_GameState->TextEntity` or `g_GameState->DirectionalLightEntity`. Wrap each creation in an `INVALID_ENTITY` check.

Example for the directional light (locate the existing creation code; the exact location depends on the current layout — likely inside a state init block):

Before:
```cpp
g_GameState->DirectionalLightEntity = g_GameState->World.CreateEntity();
g_GameState->World.AddComponent(g_GameState->DirectionalLightEntity, LightningComponent{
    .Type = LightningType::Directional,
    /* ... */
});
```

After:
```cpp
if (g_GameState->DirectionalLightEntity == INVALID_ENTITY) {
    g_GameState->DirectionalLightEntity = g_GameState->World.CreateEntity();
    g_GameState->World.AddComponent(g_GameState->DirectionalLightEntity, LightningComponent{
        .Type = LightningType::Directional,
        /* ... */
    });
}
```

Apply the same wrapping pattern for `TextEntity` and any other entity creation site that should be a one-time init.

- [ ] **Step 5: Build game and editor**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target game
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
```

Expected: both build cleanly. Compile errors here usually mean a missed reference (search the file for any remaining bare `gKeysDown`, `g_DayNightCycleSeconds`, etc.).

- [ ] **Step 6: Smoke-test the editor**

Run:
```
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/editor.exe
```

Expected: editor launches. Game logic still works — directional light still rotates, text rotates, mouse aim toggles. No regressions.

Close the editor.

- [ ] **Step 7: Commit**

```
git add src/game/include/Game.h src/game/src/Game.cpp
git commit -m "Migrate game file-static globals into GameState"
```

---

## Task 11: Decouple Game.cpp from WorldManager; move world load to editor

Game.cpp's only editor-side reach-in is `#include "../../editor/src/utilities/WorldManager.h"` + one call. Move the call to `GameThread::RunLoop`.

**Files:**
- Modify: `src/game/src/Game.cpp`
- Modify: `src/editor/src/threading/GameThread.cpp`

- [ ] **Step 1: Remove the WorldManager include and call from Game.cpp**

In `src/game/src/Game.cpp`:

Delete the include line:
```cpp
#include "../../editor/src/utilities/WorldManager.h"
```

Find the existing call site (near `game.cpp:72`):

```cpp
if (WorldManager::LoadWorldSnapshot(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH, &g_GameState->World)) {
    /* ... */
}
```

Delete that entire `if` block. The world is now loaded by editor-side code before `GameUpdate` ever runs.

- [ ] **Step 2: Add world load to GameThread before main loop**

In `src/editor/src/threading/GameThread.cpp`, add `#include "WorldManager.h"` near the other includes at the top.

In `GameThread::RunLoop`, find the block that constructs `GameState gameState{}`. After that line and BEFORE the `m_WorkerStop.store(...)` line, add:

```cpp
// Load default world before any GameUpdate call. Guarded by WorldLoaded so
// reload (which doesn't reconstruct GameState) doesn't reload the world.
if (!gameState.WorldLoaded) {
    if (WorldManager::LoadWorldSnapshot(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH, &gameState.World)) {
        gameState.WorldLoaded = true;
        SM_TRACE("GameThread: default world loaded from '%s'", WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH);
    } else {
        SM_WARN("GameThread: default world '%s' not loaded (file missing or invalid)", WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH);
    }
}
```

- [ ] **Step 3: Build editor and game**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target game
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
```

Expected: both build cleanly.

- [ ] **Step 4: Verify Game.cpp has zero editor-header references**

Run (PowerShell):
```
Select-String -Path src/game/src/Game.cpp -Pattern 'editor/' -CaseSensitive
```

Expected: no matches. Game.cpp now depends only on `Game.h`, `ApplicationContext.h`, `ECS.h`, `Input.h`, glm.

- [ ] **Step 5: Smoke-test the editor**

Run:
```
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/editor.exe
```

Expected: world loads from `assets/world.json` (or whichever path `DEFAULT_WORLD_SNAPSHOT_PATH` resolves to). If the file doesn't exist, editor warns and continues. Game logic still works.

Close the editor.

- [ ] **Step 6: Commit**

```
git add src/game/src/Game.cpp src/editor/src/threading/GameThread.cpp
git commit -m "Move WorldManager::LoadWorldSnapshot from Game.cpp to GameThread"
```

---

## Task 12: Create `GameLibrary` class scaffold

RAII wrapper around `HMODULE` + symbol pointers + timestamped-copy logic. Built but not yet integrated.

**Files:**
- Create: `src/editor/src/threading/GameLibrary.h`
- Create: `src/editor/src/threading/GameLibrary.cpp`
- Modify: `src/editor/CMakeLists.txt`

- [ ] **Step 1: Create the header**

Create `src/editor/src/threading/GameLibrary.h`:

```cpp
#pragma once

#include <windows.h>
#include <string>

#include "Game.h"  // GameState + GameUpdateFunc/GameResizeFunc/GameExitFunc/GameGetVersionFunc + GAME_API_VERSION

/**
 * @brief RAII wrapper around a dynamically-loaded Game.dll. Owns HMODULE,
 *        resolved symbol pointers, and the timestamped-copy filename actually
 *        loaded. Single-thread (GameThread) owner.
 */
class GameLibrary {
public:
    GameLibrary() = default;
    ~GameLibrary();

    GameLibrary(const GameLibrary&) = delete;
    GameLibrary& operator=(const GameLibrary&) = delete;
    GameLibrary(GameLibrary&&) = delete;
    GameLibrary& operator=(GameLibrary&&) = delete;

    /**
     * @brief Loads `sourceDllPath` via a timestamped copy, resolves symbols,
     *        validates GAME_API_VERSION. On reload, invokes prior module's
     *        GameExit(state) before FreeLibrary, then swaps.
     * @return true on success; false if load/validation failed and the previous
     *         module (if any) is preserved.
     */
    bool LoadOrReload(const std::string& sourceDllPath, GameState* state);

    /** @brief True when GameUpdate is installed. */
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

- [ ] **Step 2: Create the implementation**

Create `src/editor/src/threading/GameLibrary.cpp`:

```cpp
#include "GameLibrary.h"

#include <chrono>
#include <filesystem>
#include <system_error>

#include "lib.h"  // SM_TRACE / SM_WARN / SM_ERROR

namespace {

std::string MakeTimestampedCopyPath(const std::string& srcDir,
                                    const std::string& baseName,
                                    uint64_t counter)
{
    const auto ts = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return srcDir + "/" + baseName + "_load_" +
           std::to_string(ts) + "_" + std::to_string(counter) + ".dll";
}

} // namespace

GameLibrary::~GameLibrary() {
    // Cannot call GameExit here — no GameState pointer at destruction.
    // Caller (GameThread shutdown) must call Unload(state) before destruction.
    if (m_Module) {
        FreeLibrary(m_Module);
        m_Module = nullptr;
    }
}

bool GameLibrary::LoadOrReload(const std::string& sourceDllPath, GameState* state) {
    namespace fs = std::filesystem;

    const fs::path srcPath(sourceDllPath);
    const auto srcDir   = srcPath.parent_path().string().empty()
                            ? std::string(".")
                            : srcPath.parent_path().string();
    const auto baseName = srcPath.stem().string();
    const auto copyPath = MakeTimestampedCopyPath(srcDir, baseName, ++m_ReloadCounter);

    std::error_code ec;
    fs::copy_file(srcPath, copyPath, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        SM_WARN("GameLibrary: copy_file('%s' -> '%s') failed: %s",
                sourceDllPath.c_str(), copyPath.c_str(), ec.message().c_str());
        return false;
    }

    HMODULE newModule = LoadLibraryA(copyPath.c_str());
    if (!newModule) {
        SM_ERROR("GameLibrary: LoadLibraryA('%s') failed (GetLastError=%lu)",
                 copyPath.c_str(), GetLastError());
        fs::remove(copyPath, ec);
        return false;
    }

    auto pVersion = reinterpret_cast<GameGetVersionFunc>(
        GetProcAddress(newModule, "GameGetVersion"));
    auto pUpdate  = reinterpret_cast<GameUpdateFunc>(
        GetProcAddress(newModule, "GameUpdate"));
    if (!pVersion || !pUpdate) {
        SM_ERROR("GameLibrary: missing required exports in '%s' (GameGetVersion=%p, GameUpdate=%p)",
                 copyPath.c_str(), (void*)pVersion, (void*)pUpdate);
        FreeLibrary(newModule);
        fs::remove(copyPath, ec);
        return false;
    }

    const uint32_t v = pVersion();
    if (v != GAME_API_VERSION) {
        SM_ERROR("GameLibrary: API version mismatch (editor=%u, dll=%u). Rebuild both targets.",
                 GAME_API_VERSION, v);
        FreeLibrary(newModule);
        fs::remove(copyPath, ec);
        return false;
    }

    auto pResize = reinterpret_cast<GameResizeFunc>(GetProcAddress(newModule, "GameResize"));
    auto pExit   = reinterpret_cast<GameExitFunc>(GetProcAddress(newModule, "GameExit"));

    if (m_Module) {
        if (m_pGameExit) m_pGameExit(state);
        FreeLibrary(m_Module);
        fs::remove(m_LoadedDllPath, ec);  // best-effort
        SM_TRACE("GameLibrary: unloaded previous module '%s'", m_LoadedDllPath.c_str());
    }

    m_Module          = newModule;
    m_LoadedDllPath   = copyPath;
    m_pGameUpdate     = pUpdate;
    m_pGameResize     = pResize;
    m_pGameExit       = pExit;
    m_pGameGetVersion = pVersion;

    SM_TRACE("GameLibrary: loaded '%s' (API v%u)", copyPath.c_str(), v);
    return true;
}

void GameLibrary::Unload(GameState* state) {
    if (!m_Module) return;
    if (m_pGameExit) m_pGameExit(state);
    FreeLibrary(m_Module);
    std::error_code ec;
    std::filesystem::remove(m_LoadedDllPath, ec);
    m_Module = nullptr;
    m_pGameUpdate     = nullptr;
    m_pGameResize     = nullptr;
    m_pGameExit       = nullptr;
    m_pGameGetVersion = nullptr;
}
```

- [ ] **Step 3: Add GameLibrary.cpp to editor target**

In `src/editor/CMakeLists.txt`, find the `add_executable(editor ...)` block. In the `# Threading` section, add `src/threading/GameLibrary.cpp`:

```cmake
    # Threading
    src/threading/PlatformThread.cpp
    src/threading/GameThread.cpp
    src/threading/RenderThread.cpp
    src/threading/GameLibrary.cpp
```

- [ ] **Step 4: Build editor and verify**

Run:
```
cmake --preset msvc-win64-vs2026-enterprise
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
```

Expected: builds cleanly. `GameLibrary.obj` produced; nothing yet uses it.

- [ ] **Step 5: Commit**

```
git add src/editor/src/threading/GameLibrary.h src/editor/src/threading/GameLibrary.cpp src/editor/CMakeLists.txt
git commit -m "Add GameLibrary RAII wrapper for dynamic game module"
```

---

## Task 13: Integrate `GameLibrary` into `GameThread`; remove direct game link

Switch the editor from import-library link to runtime `LoadLibrary` via `GameLibrary`.

**Files:**
- Modify: `src/editor/src/threading/GameThread.h`
- Modify: `src/editor/src/threading/GameThread.cpp`
- Modify: `src/editor/CMakeLists.txt`

- [ ] **Step 1: Add GameLibrary member to GameThread.h**

In `src/editor/src/threading/GameThread.h`, add the include near the others:

```cpp
#include "GameLibrary.h"
```

Inside the `GameThread` class (next to other private members), add:

```cpp
    GameLibrary m_GameLib;
```

- [ ] **Step 2: Replace direct GameUpdate call in GameThread.cpp**

In `src/editor/src/threading/GameThread.cpp`, after the initial `GameState gameState{}` construction and BEFORE the worker thread spawn, add the initial load:

```cpp
// Initial load of Game.dll. If it fails, editor still runs without game logic
// until the file watcher (installed below) picks up a subsequent rebuild.
if (!m_GameLib.LoadOrReload("Game.dll", &gameState)) {
    SM_ERROR("GameThread: initial Game.dll load failed. "
             "Editor will run without game logic until Game.dll becomes loadable.");
}
```

Find the existing call to `GameUpdate(&gameState)` (around line 271 of the current file). Replace with:

```cpp
if (m_GameLib.IsValid()) {
    m_GameLib.Update(&gameState);
}
```

Find the existing call to `GameExit(&gameState)` near the end of `RunLoop` (during shutdown). Replace with:

```cpp
m_GameLib.Unload(&gameState);
```

- [ ] **Step 3: Drop direct `game` link from editor CMakeLists**

In `src/editor/CMakeLists.txt`, find:

```cmake
target_link_libraries(editor PRIVATE CommonHeaders GameHeaders ecs freetype Tracy::TracyClient glm::glm glfw nvrhi nvrhi_d3d12 nvrhi_d3d11 d3dcompiler dxgi dxcompiler nvrhi_vk imgui ImGuizmo assimp::assimp nlohmann_json::nlohmann_json game)
```

Change to (remove trailing `game`):

```cmake
target_link_libraries(editor PRIVATE CommonHeaders GameHeaders ecs freetype Tracy::TracyClient glm::glm glfw nvrhi nvrhi_d3d12 nvrhi_d3d11 d3dcompiler dxgi dxcompiler nvrhi_vk imgui ImGuizmo assimp::assimp nlohmann_json::nlohmann_json)
```

The `add_dependencies(editor game)` line at the bottom of the file STAYS — it ensures Game.dll is built before editor.exe runs.

- [ ] **Step 4: Re-configure and rebuild editor**

Run:
```
cmake --preset msvc-win64-vs2026-enterprise
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
```

Expected: editor builds. The import library `Game.lib` is no longer referenced by `editor.exe` — only `LoadLibrary("Game.dll")` at runtime.

- [ ] **Step 5: Smoke-test editor**

Run:
```
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/editor.exe
```

Expected: editor launches. Console log shows:
```
TRACE: GameLibrary: loaded 'Game_load_<timestamp>_1.dll' (API v1)
```

Game runs normally. Move ImGui camera, observe text/light animations.

Close editor. Verify the `Game_load_*.dll` timestamped file is left in `bin/Debug/` (cleanup happens on next build, not on exit).

- [ ] **Step 6: Commit**

```
git add src/editor/src/threading/GameThread.h src/editor/src/threading/GameThread.cpp src/editor/CMakeLists.txt
git commit -m "Load Game.dll dynamically via GameLibrary; drop direct link"
```

---

## Task 14: Add file watcher + reload-pending flag drain

`filewatch::FileWatch` runs on a background thread; sets `m_ReloadPending` atomic; `GameThread` drains at tick boundary.

**Files:**
- Modify: `src/editor/src/threading/GameThread.h`
- Modify: `src/editor/src/threading/GameThread.cpp`

- [ ] **Step 1: Add watcher + atomic flag members to GameThread.h**

In `src/editor/src/threading/GameThread.h`, add includes near the others:

```cpp
#include <atomic>
#include <memory>
#include "FileWatch.h"
```

Inside the `GameThread` class private members, near `m_GameLib`, add:

```cpp
    std::atomic<bool> m_ReloadPending{false};
    std::unique_ptr<filewatch::FileWatch<std::string>> m_GameDllWatcher;
```

- [ ] **Step 2: Add watcher setup in GameThread.cpp**

In `src/editor/src/threading/GameThread.cpp`, add `#include <regex>` near the top.

After the initial `m_GameLib.LoadOrReload(...)` call but BEFORE the main `while (Running())` loop, add:

```cpp
// File watcher: detects Game.dll changes on a background thread.
// Callback sets m_ReloadPending; GameThread drains it at the top of each tick.
// CWD at runtime is RUNTIME_DIR (set via VS_DEBUGGER_WORKING_DIRECTORY in CMake).
try {
    m_GameDllWatcher = std::make_unique<filewatch::FileWatch<std::string>>(
        std::string("."),                       // watch CWD = RUNTIME_DIR
        std::regex(R"(^Game\.dll$)"),           // exact filename match
        [this](const std::string& /*file*/, const filewatch::Event evt) {
            if (evt == filewatch::Event::modified ||
                evt == filewatch::Event::added) {
                m_ReloadPending.store(true, std::memory_order_release);
            }
        });
    SM_TRACE("GameThread: filewatch installed on './Game.dll'");
} catch (const std::exception& e) {
    SM_ERROR("GameThread: filewatch setup failed: %s. Hot-reload disabled.", e.what());
}
```

- [ ] **Step 3: Drain reload flag at top of tick**

In `src/editor/src/threading/GameThread.cpp`, find the `while (Running())` loop. At the very top of the loop body, BEFORE the existing settings/input drain logic, add:

```cpp
// Drain reload flag BEFORE input/commands/game logic so the rest of the tick
// runs on the new code.
if (m_ReloadPending.exchange(false, std::memory_order_acquire)) {
    ZoneScopedN("Game:Reload");
    if (m_GameLib.LoadOrReload("Game.dll", &gameState)) {
        SM_TRACE("GameThread: Game.dll reloaded successfully");
    }
    // On failure, GameLibrary already logged and kept the previous module.
}
```

- [ ] **Step 4: Build editor**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
```

Expected: builds cleanly.

- [ ] **Step 5: Smoke-test the reload path**

Open two terminals.

Terminal 1 (run editor):
```
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/editor.exe
```

Verify: editor launches, console shows `filewatch installed on './Game.dll'`, game runs.

Terminal 2 (modify and rebuild game while editor runs):

1. Open `src/game/src/Game.cpp` and change a trivial value the game uses each tick. Suggested change: in the day-night cycle block, change the `glm::vec3` direction formula multiplier or add a print like `SM_TRACE("[GAMEDLL] hot reload alive");` inside `GameUpdate`.
2. Rebuild game:
   ```
   cmake --build --preset msvc-win64-vs2026-enterprise --target game
   ```

Switch back to Terminal 1. Within ~1 second of the rebuild completing, the editor console should show:

```
TRACE: GameLibrary: unloaded previous module 'Game_load_<old_ts>_<n>.dll'
TRACE: GameLibrary: loaded 'Game_load_<new_ts>_<n+1>.dll' (API v1)
TRACE: GameThread: Game.dll reloaded successfully
```

If you added the `SM_TRACE` print, it should start appearing in subsequent log lines.

Close the editor.

- [ ] **Step 6: Revert the smoke-test edit to Game.cpp**

If you added a `SM_TRACE("[GAMEDLL] hot reload alive")` for testing, remove it now and rebuild:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target game
```

- [ ] **Step 7: Commit**

```
git add src/editor/src/threading/GameThread.h src/editor/src/threading/GameThread.cpp
git commit -m "Add file watcher + reload-flag drain for game hot-reload"
```

---

## Task 15: Final verification + documentation

End-to-end sanity pass on the full hot-reload workflow + documentation updates.

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Clean build all targets**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target ecs
cmake --build --preset msvc-win64-vs2026-enterprise --target game
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
cmake --build --preset msvc-win64-vs2026-enterprise --target test_ecs
```

Expected: all four targets build cleanly.

- [ ] **Step 2: Run test_ecs against the shipped ecs.dll**

Run:
```
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_ecs.exe
```

Expected: `All ECS tests passed.` Exit code 0. The test now exercises ecs.dll's instantiations (same binary the editor uses).

- [ ] **Step 3: Full manual smoke test**

Start editor:
```
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/editor.exe
```

Check each:
- [ ] Editor launches; world loads (or warns if no world.json).
- [ ] Console shows `GameLibrary: loaded 'Game_load_<ts>_1.dll' (API v1)` once on startup.
- [ ] Console shows `filewatch installed on './Game.dll'`.
- [ ] Directional light rotates as expected; text rotates and changes color.
- [ ] ImGui inspector lists entities; TextEntity and DirectionalLightEntity have stable IDs.
- [ ] Press T key — mouse aim toggles. Verify game responds.

While editor is running, in a separate terminal:

```
cmake --build --preset msvc-win64-vs2026-enterprise --target game
```

Switch back to editor:
- [ ] Console shows reload log lines within ~1 second.
- [ ] Game continues running. Light still in same position. Directional light entity ID unchanged in inspector.
- [ ] Hold W key during the rebuild. After reload, W still registers as held (KeysDown[KEY_W] preserved).

Close editor.

- [ ] **Step 4: Verify cleanup behavior**

Check `bin/Debug/` directory:
- [ ] At most one `Game_load_*.dll` file remains (the last-loaded one; PRE_BUILD wiped earlier copies on the rebuild).

Run another game build:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target game
```

Check `bin/Debug/`:
- [ ] All `Game_load_*.dll` files are gone (PRE_BUILD wiped them). Only `Game.dll` (the build output) remains.

- [ ] **Step 5: Verify API mismatch path**

Test the version mismatch rejection:

1. Temporarily edit `src/game/include/Game.h`, change:
   ```cpp
   #define GAME_API_VERSION 1u
   ```
   to:
   ```cpp
   #define GAME_API_VERSION 99u
   ```

2. Rebuild game ONLY (do not rebuild editor):
   ```
   cmake --build --preset msvc-win64-vs2026-enterprise --target game
   ```

3. Start editor (or have it running and trigger reload):
   ```
   ./out/build/msvc-win64-vs2026-enterprise/bin/Debug/editor.exe
   ```

4. Verify console shows:
   ```
   ERROR: GameLibrary: API version mismatch (editor=1, dll=99). Rebuild both targets.
   ```
   And editor stays usable (initial load failure mode: empty game logic, but UI works).

5. Revert `GAME_API_VERSION` back to `1u` and rebuild game:
   ```
   cmake --build --preset msvc-win64-vs2026-enterprise --target game
   ```

   Editor reload-watcher picks up the rebuild; game starts working.

Close editor.

- [ ] **Step 6: Update CLAUDE.md**

In `CLAUDE.md`, in an appropriate section (suggest near the existing "Build & run" or as a new top-level section), add:

```markdown
## Hot-reloading the game library

`game` is built as a SHARED library (`Game.dll`) and is loaded at runtime by the editor via `GameLibrary` (`src/editor/src/threading/GameLibrary.{h,cpp}`). A `filewatch::FileWatch` on `Game.dll` triggers a reload between ticks when the file changes on disk.

Typical iteration:

1. Edit `src/game/src/Game.cpp`.
2. `cmake --build --preset msvc-win64-vs2026-enterprise --target game`
3. Editor reloads automatically within ~1 second. Console logs the new timestamped DLL filename and the API version that loaded.

State preservation: cross-tick game state lives in `GameState` (`src/game/include/Game.h`) which is editor-owned. The DLL holds only a pointer. File-static globals in `Game.cpp` are per-tick scratch only and reset on reload — intentional.

Rules:

- **Change `.cpp` only** → hot-reload works. No editor restart.
- **Change `Game.h` (struct layout, new export, etc.)** → bump `GAME_API_VERSION`. Rebuild **both** game and editor. Restart editor (the running `editor.exe` still has the old `GameState` layout linked in).
- **Change `ECS.h` (new component type, etc.)** → rebuild `ecs.dll`, editor, and game. Restart editor.

ECS code lives in `ecs.dll` (`src/ecs/`). All `ComponentArray<T>` template instantiations are explicit, driven by the `ECS_FOR_EACH_REGISTERED_COMPONENT` X-macro in `ECS.h`. Adding a new component type:

1. Declare the struct in `ECS.h`.
2. Add `X(NewType)` to `ECS_FOR_EACH_REGISTERED_COMPONENT`.
3. Add handling to `ECSCommandProcessor::ApplyComponentCommand` and `RemoveComponentByType` in `ApplicationContext.h`.
4. (Optional) Add inspector UI in `ImGuiRenderer.cpp`.
```

- [ ] **Step 7: Commit the documentation update**

```
git add CLAUDE.md
git commit -m "Document game hot-reload workflow in CLAUDE.md"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Tasks |
|---|---|
| Architecture overview (Pattern Z: ecs.dll + game.dll) | T2 (ecs target), T4-T7 (ECS extraction), T8 (game SHARED), T12-T13 (GameLibrary integration) |
| Game.dll API contract (GAME_API_VERSION, exports) | T9 (version constant + GetVersion update) |
| GameLibrary class (LoadOrReload, Unload) | T12 (class), T13 (integration) |
| File watcher + reload coordination | T14 |
| State migration (file-static → GameState) | T10 (struct + game.cpp rewrites), T11 (WorldManager move) |
| ecs.dll build setup (ECS_API, X-macro, instantiations) | T2 (target), T3 (macros), T4-T7 (extraction) |
| Game.dll build setup (SHARED, /Z7, PRE_BUILD) | T8 |
| CRT lockdown | T1 |
| Error handling (version mismatch, load failure) | T12 (GameLibrary impl), T15 step 5 (verification) |
| Testing | T15 (full smoke checklist + API mismatch path) |

All spec sections accounted for.

**Placeholder scan:** No `TBD`/`TODO`/`fill in details` in plan content. Body-relocation tasks reference existing code by name + location and provide the full target body for the new location. Manual smoke-test steps are concrete with expected console output.

**Type consistency:**
- `GameLibrary` interface (`LoadOrReload`, `Unload`, `IsValid`, `Update`, `Resize`) consistent across T12 (creation) and T13 (use).
- `GAME_API_VERSION` macro consistent across T9 (definition) and T12 (consumer in `LoadOrReload`).
- `m_GameLib`, `m_ReloadPending`, `m_GameDllWatcher` member names consistent across T13 and T14.
- `ECS_FOR_EACH_REGISTERED_COMPONENT` X-macro use consistent across T3 (definition) and T4-T7 (consumers).
- File path `"Game.dll"` (relative to CWD = RUNTIME_DIR) consistent across T13 and T14.
- `gameState.WorldLoaded` field name consistent across T10 (struct addition) and T11 (consumer).

No mismatches.
