# Stable Asset Identity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `MeshComponent`/`MaterialComponent` reference assets by a stable logical key (virtual path) resolved through an `AssetRegistry` seam, with the runtime handle = `uint64` hash of that key, so adding/reordering assets no longer corrupts saved `world.json` references.

**Architecture:** Six tasks: (A) foundation headers `AssetKey.h` (pure hash) + `AssetRegistry.h` (fn-ptr seam) + exported global; (B) mechanical widen of all asset handles/ids `uint32→uint64`; (C) MeshSystem end-to-end keyed registry (system + ring + game-load plumbing + editor picker); (D) MaterialSystem end-to-end keyed registry; (E) key-based serialization + install the registry + round-trip tests; (F) full regression. Each task builds and passes tests on its own.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-community`), CMake, nlohmann/json, NVRHI, Dear ImGui (editor only). Reuses the codebase's fn-ptr service-bridge idiom (`NavServices`/`EditorUI`) and the COW-snapshot / SPSC-ring infrastructure.

**Scope:** Implements `docs/superpowers/specs/2026-06-07-stable-asset-identity-design.md`. Touches `ECS.h` + `ApplicationContext.h` ⇒ rebuild **ecs + Engine + editor + game** and **restart the editor**. No `GAME_API_VERSION` bump (`GameState` layout unchanged).

> **Branch:** Work happens on `feat/asset-id-stability` (already created off `main`). Stay on it.

---

## Background facts (verified — current code)

- **Line numbers are approximate — locate every edit by the quoted code.**
- Handles (`ApplicationContext.h:114-116`): `struct ModelHandle/MeshHandle/MaterialHandle { uint32_t Index; };`. `MeshComponent.MeshId` / `MaterialComponent.MaterialId` are `uint32_t` (`ECS.h:59-77`). `SubMesh.MaterialIndex` is `uint32_t` (runtime-only, NOT persisted — leave its width but it must carry a material handle, so widen it too).
- `RendererCommand` (`ApplicationContext.h:84-112`) has an **anonymous `union`** of POD request structs (`MeshRequest`, `MaterialRequest`, …). **A `std::string` cannot go in the union** — the asset key rides as a fixed `char Key[256]` inside `MeshRequest`/`MaterialRequest`.
- `RendererResponse` (`:123-137`) returns `Mesh.Handle` / `Material.Handle`. With hashed handles, `Handle.Index` IS the stable id — existing wiring `m.MeshId = response.Mesh.Handle.Index` (`GameThread.cpp:458`, `:471`) keeps working once widened.
- `MeshSystem::AddMesh` (`MeshSystem.cpp:67`) and `MaterialSystem::AddMaterial` (`MaterialSystem.cpp:22`) push to a `std::vector` and return `Handle{ static_cast<uint32_t>(m_*.size()) }`; failure returns `Handle{ UINT32_MAX }`. Lookups (`GetMeshResources`/`GetMeshBounds`/`IsValidMeshId`/`GetMeshCpuData`/`GetMaterialResources`) index the vector by id and warn if `id >= size`. `MissingMesh`/`MissingMaterial` constants = `0` (`MeshSystem.h:40`, `MaterialSystem.h:34`); material slot 0 is the magenta-checkerboard missing texture (`MaterialSystem.cpp:11-17`).
- `MeshVertex` is in `ApplicationContext.h:67-72`.
- Model load is async: startup `directory_iterator("assets/models")` (`GameThread.cpp:127-140`) → `EnqueueModelLoadJob(ticket, objPath, mtlBaseDir)`; worker fills `ModelLoadResult` (`GameThread.h:41-54`, carries `ticketId`, verts/indices/subMeshes, one texture's pixels — NO path); game drains, builds `RendererCommand{RequestMesh/RequestMaterial}` (`GameThread.cpp:346-428`), pushes on `GRCommandRing`; response drain (`:430-485`) sets the component ids + calls `NavMeshSystem::StoreMeshCpuData(handle.Index, …)`. `ModelLoadJob` (`GameThread.h:34-39`) / `ModelLoadResult` are plain structs → `std::string` members OK.
- NavMesh CPU cache (`NavMeshSystem.h:78-106`, `.cpp:291-309`) is keyed by `uint32_t meshId` (`std::unordered_map<uint32_t, CachedMesh>`).
- `ECS_API` macro (`ECS.h:31-40`): `__declspec(dllexport)` when `ECS_EXPORTS` (set for the `ecs` target). Pattern: declare `ECS_API` in a common header, define WITHOUT `ECS_API` in a `.cpp` under `src/ecs/src/` (e.g. `ecs.cpp`, which also defines `BuiltinComponentTypes` at `:113`). `ecs` sources: `systems.cpp`, `EcsLogSink.cpp`, `ecs.cpp`, `ComponentSerializers.cpp`.
- `world.json` save (`MainMenuBar.cpp:25-29`, `WorldManager::SaveWorldSnapshot`) runs on the editor/ImGui (Render) thread directly against the `ctx.WorldSnapshot` — same thread MeshSystem/MaterialSystem live on, so the registry reverse-lookup is safe there. Load (`WorldManager::LoadWorldSnapshot`) runs on GameThread and needs only the pure hash.
- Editor pickers: `MeshEditor.cpp` / `MaterialEditor.cpp` build `"Mesh i"`/`"Material i"` dropdowns over `GetMeshCount()`/`GetMaterialCount()` and assign the index. `ctx.MeshSys` / `ctx.MatSys` are the systems.

## Type/symbol contract (keep exact)

- `src/common/include/AssetKey.h`: `uint64_t AssetKeyHash(std::string_view)`, `constexpr uint64_t kMissingAssetHandle = 0`, `std::string NormalizeAssetKey(std::string path)`.
- `src/common/include/AssetRegistry.h`: `struct AssetRegistry { std::string (*MeshKeyForHandle)(uint64_t); std::string (*MaterialKeyForHandle)(uint64_t); };` + `ECS_API void SetAssetRegistry(const AssetRegistry*)` + `ECS_API const AssetRegistry* GetAssetRegistry()`.
- Handles + `MeshId`/`MaterialId` + `SubMesh.MaterialIndex` + NavMesh cache key + all id params: `uint64_t`. Failure sentinel: `UINT64_MAX`.
- `MeshSystem`/`MaterialSystem`: `AddMesh(std::string key, …)` / `AddMaterial(std::string key, …)`; `std::string KeyForHandle(uint64_t) const`; `std::vector<std::pair<uint64_t,std::string>> GetAssetList() const`; internal `std::unordered_map<uint64_t,uint32_t> m_SlotByHandle`.
- `MeshRequest`/`MaterialRequest`: `char Key[256];`.
- `ModelLoadJob`/`ModelLoadResult`: `std::string assetKey;`.
- world.json keys: `"MeshKey"` (replaces `"MeshId"`), `"MaterialKey"` (replaces `"MaterialId"`).

---

### Task A: Foundation headers — `AssetKey.h`, `AssetRegistry.h`, exported global + tests

**Files:**
- Create: `src/common/include/AssetKey.h`
- Create: `src/common/include/AssetRegistry.h`
- Modify: `src/ecs/src/ecs.cpp`
- Test: `tests/test_assetkey.cpp` (new) + `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test (`tests/test_assetkey.cpp`)**
```cpp
#include <cstdio>
#include <string>
#include "AssetKey.h"

static int g_Failures = 0;
#define EXPECT(cond) do { if(!(cond)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); ++g_Failures; } } while(0)

int main() {
    // Determinism: same key -> same hash.
    EXPECT(AssetKeyHash("models/tree.obj") == AssetKeyHash("models/tree.obj"));
    // Distinct keys -> distinct hashes (sample set).
    EXPECT(AssetKeyHash("models/tree.obj") != AssetKeyHash("models/rock.obj"));
    EXPECT(AssetKeyHash("textures/bark.png") != AssetKeyHash("models/tree.obj"));
    // Non-empty key never collides with the reserved Missing handle.
    EXPECT(AssetKeyHash("models/tree.obj") != kMissingAssetHandle);
    // Normalize: backslashes -> forward, strip a leading assets/ prefix.
    EXPECT(NormalizeAssetKey("assets\\models\\tree.obj") == "models/tree.obj");
    EXPECT(NormalizeAssetKey("assets/models/tree.obj") == "models/tree.obj");
    EXPECT(NormalizeAssetKey("models/tree.obj") == "models/tree.obj");

    if (g_Failures == 0) std::printf("All asset-key tests passed.\n");
    return g_Failures ? 1 : 0;
}
```
Register the target in `tests/CMakeLists.txt` — mirror an existing simple test target (e.g. find the `test_compserial` / `test_inputdrain` block and copy its shape). Add:
```cmake
add_executable(test_assetkey test_assetkey.cpp)
target_link_libraries(test_assetkey PRIVATE ecs)
target_include_directories(test_assetkey PRIVATE ${CMAKE_SOURCE_DIR}/src/common/include)
```
(Match the include/link style of the neighboring test targets in that file; `AssetKey.h` is header-only so linking `ecs` is for the include path + consistency.)

- [ ] **Step 2: Configure + build the test — confirm it FAILS**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_assetkey
```
Expected: COMPILE ERROR — `AssetKey.h` not found / `AssetKeyHash` undeclared. (TDD red.)

- [ ] **Step 3: Create `src/common/include/AssetKey.h`**
```cpp
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Stable asset identity helpers. Pure + header-only so every module (ecs.dll, Engine, editor,
// game, tests) derives the SAME handle from the SAME logical key with no shared state. The logical
// key is a virtual path under assets/ (forward-slashed), e.g. "models/tree.obj" — the same
// addressing a future VFS uses. See docs/superpowers/specs/2026-06-07-stable-asset-identity-design.md.

// Reserved handle for the Missing mesh / magenta-checkerboard default material (slot 0).
inline constexpr uint64_t kMissingAssetHandle = 0ull;

// 64-bit FNV-1a over the key bytes. The empty key maps to kMissingAssetHandle explicitly (callers
// special-case empty before hashing); a non-empty key effectively never hashes to 0.
inline uint64_t AssetKeyHash(std::string_view key) {
    if (key.empty()) return kMissingAssetHandle;
    uint64_t h = 1469598103934665603ull;            // FNV offset basis
    for (unsigned char c : key) { h ^= c; h *= 1099511628211ull; } // FNV prime
    return h;
}

// Turn a filesystem path into a logical asset key: backslashes -> forward slashes, strip a leading
// "assets/" or "./". Pure string work (no <filesystem> dependency).
inline std::string NormalizeAssetKey(std::string path) {
    for (char& c : path) if (c == '\\') c = '/';
    if (path.rfind("./", 0) == 0) path.erase(0, 2);
    if (path.rfind("assets/", 0) == 0) path.erase(0, 7);
    return path;
}
```

- [ ] **Step 4: Build + run the test — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community --target test_assetkey
./out/build/msvc-win64-vs2026-community/bin/Debug/test_assetkey.exe
```
Expected: `All asset-key tests passed.`

- [ ] **Step 5: Create `src/common/include/AssetRegistry.h`**
```cpp
#pragma once
#include <cstdint>
#include <string>
#include "ECS.h"   // ECS_API

// Resolution seam between a stable logical asset key and the runtime handle. Today backed by
// MeshSystem/MaterialSystem; a future VFS re-backs THIS interface and nothing above it changes.
// Forward resolution (key->handle) is pure AssetKeyHash and does NOT need the registry; the
// registry provides the REVERSE map (handle->key) for serialization + editor display, which only
// the owning systems know. Fn-ptr struct, same idiom as NavServices/EditorUI.
struct AssetRegistry {
    std::string (*MeshKeyForHandle)(uint64_t handle);      // "" if unknown
    std::string (*MaterialKeyForHandle)(uint64_t handle);  // "" if unknown
};

// Process-wide pointer, set by Engine at init (RenderThread owns the systems). Null where no asset
// systems exist (unit tests) — callers fall back gracefully (empty key). Defined in ecs.dll so the
// common-header serializers can reach it.
ECS_API void SetAssetRegistry(const AssetRegistry* reg);
ECS_API const AssetRegistry* GetAssetRegistry();
```

- [ ] **Step 6: Define the global in `src/ecs/src/ecs.cpp`**

Add an include near the top of `ecs.cpp` (alongside its existing includes):
```cpp
#include "AssetRegistry.h"
```
Add the definition (e.g. just after `BuiltinComponentTypes()` at `ecs.cpp:113-122`):
```cpp
static const AssetRegistry* g_AssetRegistry = nullptr;
void SetAssetRegistry(const AssetRegistry* reg) { g_AssetRegistry = reg; }
const AssetRegistry* GetAssetRegistry() { return g_AssetRegistry; }
```

- [ ] **Step 7: Build `ecs` — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community --target ecs --target test_assetkey
./out/build/msvc-win64-vs2026-community/bin/Debug/test_assetkey.exe
```
Expected: builds clean; test still passes.

- [ ] **Step 8: Commit**
```
git -C C:/dev/clang-examples add src/common/include/AssetKey.h src/common/include/AssetRegistry.h src/ecs/src/ecs.cpp tests/test_assetkey.cpp tests/CMakeLists.txt
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(asset): AssetKey hash + AssetRegistry seam (foundation) + unit test"
```

---

### Task B: Widen asset handles + ids to `uint64`

**Files:** `src/common/include/ApplicationContext.h`, `src/common/include/ECS.h`, `src/engine/src/rendering/MeshSystem.{h,cpp}`, `src/engine/src/rendering/MaterialSystem.{h,cpp}`, `src/engine/src/rendering/Renderer.{h,cpp}`, `src/engine/src/navigation/NavMeshSystem.{h,cpp}`, plus build-driven fixups in render passes / editor.

Pure mechanical widening — NO behavior change (handle is still the slot index, just 64-bit). The tree must build and all tests pass at the end.

- [ ] **Step 1: Widen the handle structs (`ApplicationContext.h:114-116`)**
```cpp
struct ModelHandle { uint64_t Index; };
struct MeshHandle { uint64_t Index; };
struct MaterialHandle { uint64_t Index; };
```

- [ ] **Step 2: Widen the component ids (`ECS.h:59-77`)**

In `MeshComponent` change `uint32_t MeshId = 0;` → `uint64_t MeshId = 0;`. In `MaterialComponent` change `uint32_t MaterialId = 0;` → `uint64_t MaterialId = 0;`. In `SubMesh` change `uint32_t MaterialIndex = 0;` → `uint64_t MaterialIndex = 0;` (it carries a material handle at runtime).

- [ ] **Step 3: Widen MeshSystem/MaterialSystem id params + sentinels**

In `MeshSystem.h` change every `uint32_t meshId` parameter (`GetMeshResources`/`GetMeshCpuData`/`IsValidMeshId`/`GetMeshBounds`) to `uint64_t meshId`, and `static constexpr uint32_t MissingMesh` → `static constexpr uint64_t MissingMesh`. In `MeshSystem.cpp` match the definitions' signatures; replace the failure `return MeshHandle{ UINT32_MAX };` with `UINT64_MAX`; the `meshId >= m_Meshes.size()` guards keep working (uint64 compared to size_t). Same widening in `MaterialSystem.{h,cpp}` (`uint32_t materialId`→`uint64_t`, `MissingMaterial` const → uint64, `UINT32_MAX`→`UINT64_MAX`). NOTE: keep `m_Meshes`/`m_Materials` as `std::vector` (slot storage stays 32-bit-indexable); only the *handle/id* type widens.

- [ ] **Step 4: Widen NavMesh cache key (`NavMeshSystem.{h,cpp}`)**

`StoreMeshCpuData(uint32_t meshId, …)` / `GetMeshCpuData(uint32_t meshId, …)` → `uint64_t meshId`; `std::unordered_map<uint32_t, CachedMesh> m_MeshCpuData;` → `std::unordered_map<uint64_t, CachedMesh>`. Match the `.cpp` definitions.

- [ ] **Step 5: Build the full tree — fix build-driven widening**
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: compile errors ONLY where a `uint32_t` local still holds a mesh/material id/handle (e.g. render passes capturing `meshComp.MeshId` into a `uint32_t`, the `BatchEntry` fields in `GBufferFillPass.cpp`, `selectedMeshId`/`selectedMateridId` panel state, `MeshEditor`/`MaterialEditor` `static_cast<int>`/`InputScalar` types, `MeshPreviewRenderer` id param). Change each such `uint32_t` to `uint64_t` (or `auto`). Keep `static_cast<int>` casts for ImGui combo indices as-is (those are slot loop counters, not handles — they iterate `0..GetMeshCount()`). Rebuild until clean. Editor combo `InputScalar(..., ImGuiDataType_U32, &MeshId)` → `ImGuiDataType_U64`.

- [ ] **Step 6: Run the suites — GREEN (no behavior change)**
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_compserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_reloadpreserve.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_playermove.exe
```
Expected: all pass (widening is behavior-preserving; `world.json` still stores a numeric id at this stage).

- [ ] **Step 7: Commit**
```
git -C C:/dev/clang-examples add -- src/common/include/ApplicationContext.h src/common/include/ECS.h src/engine/src/rendering/MeshSystem.h src/engine/src/rendering/MeshSystem.cpp src/engine/src/rendering/MaterialSystem.h src/engine/src/rendering/MaterialSystem.cpp src/engine/src/rendering/Renderer.h src/engine/src/rendering/Renderer.cpp src/engine/src/navigation/NavMeshSystem.h src/engine/src/navigation/NavMeshSystem.cpp
git -C C:/dev/clang-examples status   # then `git add` any additional build-driven files (render passes, editor panels, preview) by exact path
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "refactor(asset): widen mesh/material handles + ids to uint64 (mechanical, no behavior change)"
```
Verify `git -C C:/dev/clang-examples show --stat HEAD` lists only the widened files (no `assets/*.json`).

---

### Task C: MeshSystem keyed registry — end-to-end

**Files:** `MeshSystem.{h,cpp}`, `Renderer.{h,cpp}`, `ApplicationContext.h` (MeshRequest.Key), `RenderThread.cpp`, `GameThread.{h,cpp}`, `MeshManagerPanel.cpp`, `MeshEditor.cpp`.

After this task, mesh references are key-stable end-to-end; materials still slot-based (Task D).

- [ ] **Step 1: Add the key field + maps to MeshSystem (`MeshSystem.h`)**

In `MeshEntry` (`MeshSystem.h:78`) add `std::string key;` as the first member. Add a private map next to `m_Meshes`:
```cpp
    std::unordered_map<uint64_t, uint32_t> m_SlotByHandle; // stable handle (hash) -> vector slot
```
Add `#include <unordered_map>` and `#include <string>` + `#include <utility>` if not present. Add public declarations:
```cpp
    // Stable logical key (virtual path) <-> runtime handle. KeyForHandle is the registry reverse
    // lookup used for serialization/editor display; forward (key->handle) is just AssetKeyHash.
    std::string KeyForHandle(uint64_t handle) const;
    std::vector<std::pair<uint64_t, std::string>> GetAssetList() const; // (handle, key) per loaded mesh
```
Change the public `AddMesh` signature (`MeshSystem.h:24`) to take the key first:
```cpp
    MeshHandle AddMesh(std::string key,
                       const MeshVertex* vertices, uint32_t vertexCount,
                       const uint32_t* indices, uint32_t indexCount,
                       SubMesh* subMeshes = nullptr, uint32_t subMeshCount = 0);
```

- [ ] **Step 2: Implement keying in `MeshSystem.cpp` AddMesh**

Change the signature to match. At the TOP of `AddMesh` (after the param-validation guard), add de-dup + handle:
```cpp
    const uint64_t handle = AssetKeyHash(key);   // key=="" -> kMissingAssetHandle (0)
    if (auto it = m_SlotByHandle.find(handle); it != m_SlotByHandle.end()) {
        return MeshHandle{ handle }; // already loaded (de-dup): return existing stable handle
    }
```
Set `entry.key = std::move(key);` when building the entry. At the end, replace the slot-based return:
```cpp
    const uint32_t slot = static_cast<uint32_t>(m_Meshes.size());
    m_Meshes.push_back(std::move(entry));
    m_SlotByHandle[handle] = slot;
    SM_TRACE("MeshSystem::AddMesh: '%s' -> handle %llu (slot %u, %u verts %u idx)",
             m_Meshes[slot].key.c_str(), (unsigned long long)handle, slot, vertexCount, indexCount);
    return MeshHandle{ handle };
```
Add `#include "AssetKey.h"` at the top of `MeshSystem.cpp`.

- [ ] **Step 3: Map handle→slot in all MeshSystem lookups**

Add a private helper:
```cpp
int32_t MeshSystem::SlotForHandle(uint64_t handle) const {
    auto it = m_SlotByHandle.find(handle);
    return it == m_SlotByHandle.end() ? -1 : static_cast<int32_t>(it->second);
}
```
(declare `int32_t SlotForHandle(uint64_t) const;` in the private section of `MeshSystem.h`). Rewrite each lookup to resolve via the map instead of treating the id as a slot. `GetMeshResources`:
```cpp
MeshSystem::MeshResources MeshSystem::GetMeshResources(uint64_t meshId) const {
    MeshResources resources{};
    const int32_t slot = SlotForHandle(meshId);
    if (slot < 0) { SM_WARN("MeshSystem::GetMeshResources: unknown mesh handle %llu", (unsigned long long)meshId); return resources; }
    const MeshEntry& entry = m_Meshes[slot];
    resources.vertexBuffer = entry.vertexBuffer;
    resources.indexBuffer  = entry.indexBuffer;
    resources.vertexCount  = entry.vertexCount;
    resources.indexCount   = entry.indexCount;
    resources.subMeshes    = std::span<const SubMesh>(entry.subMeshes);
    resources.valid        = true;
    return resources;
}
```
Apply the same `SlotForHandle` resolution to `GetMeshCpuData`, `GetMeshBounds`, and `IsValidMeshId`:
```cpp
bool MeshSystem::IsValidMeshId(uint64_t meshId) const { return SlotForHandle(meshId) >= 0; }
```
`GetMeshCount()` stays `m_Meshes.size()` (count of loaded meshes). Implement `KeyForHandle` + `GetAssetList`:
```cpp
std::string MeshSystem::KeyForHandle(uint64_t handle) const {
    const int32_t slot = SlotForHandle(handle);
    return slot < 0 ? std::string() : m_Meshes[slot].key;
}
std::vector<std::pair<uint64_t,std::string>> MeshSystem::GetAssetList() const {
    std::vector<std::pair<uint64_t,std::string>> out;
    for (const auto& [h, slot] : m_SlotByHandle) out.emplace_back(h, m_Meshes[slot].key);
    return out;
}
```
**`RecreateGpuResources`/`DestroyGpuResources`**: they preserve slots + CPU caches; `m_SlotByHandle` and `entry.key` live on the entries / map and are untouched by GPU-buffer recreation — confirm they aren't cleared. The internal default-mesh `AddMesh` caller (`MeshSystem.cpp:61`) now must pass a key — pass `""` so the default mesh gets `handle 0` (`MissingMesh`):
```cpp
        AddMesh("", vertices.data(), static_cast<uint32_t>(vertices.size()),
                 indices.data(), static_cast<uint32_t>(indices.size()), nullptr, 0);
```

- [ ] **Step 4: Renderer forwarder (`Renderer.{h,cpp}`)**

`Renderer.h:111`:
```cpp
    MeshHandle AddMesh(std::string key, const MeshVertex* vertices, uint32_t vertexCount,
                       const uint32_t* indices, uint32_t indexCount, SubMesh* subMeshes = nullptr, uint32_t subMeshCount = 0);
```
`Renderer.cpp:451`:
```cpp
MeshHandle Renderer::AddMesh(std::string key, const MeshVertex* vertices, uint32_t vertexCount,
                              const uint32_t* indices, uint32_t indexCount, SubMesh* subMeshes, uint32_t subMeshCount) {
    return m_MeshSystem.AddMesh(std::move(key), vertices, vertexCount, indices, indexCount, subMeshes, subMeshCount);
}
```

- [ ] **Step 5: Carry the key on the ring (`ApplicationContext.h` MeshRequest)**

In the `MeshRequest` struct inside the `RendererCommand` union (`:93-100`) add a fixed buffer (NOT a std::string — it's a union):
```cpp
        struct {
            MeshVertex* Vertices;
            size_t VertexCount;
            uint32_t* Indices;
            size_t IndexCount;
            SubMesh* SubMeshes;
            size_t SubMeshCount;
            char Key[256];   // logical asset key (virtual path); empty => default mesh
        } MeshRequest;
```

- [ ] **Step 6: RenderThread passes the key (`RenderThread.cpp` RequestMesh case)**

In the `RequestMesh` case (`:104-126`), change the `AddMesh` call to pass the key:
```cpp
                    const auto meshHandle = m_Renderer->AddMesh(
                        std::string(cmd.MeshRequest.Key),
                        cmd.MeshRequest.Vertices, static_cast<uint32_t>(cmd.MeshRequest.VertexCount),
                        cmd.MeshRequest.Indices, static_cast<uint32_t>(cmd.MeshRequest.IndexCount),
                        cmd.MeshRequest.SubMeshes, static_cast<uint32_t>(cmd.MeshRequest.SubMeshCount)
                    );
```
Change the validity check `meshHandle.Index != UINT32_MAX` → `!= UINT64_MAX`.

- [ ] **Step 7: GameThread derives + threads the key**

(a) `ModelLoadJob` (`GameThread.h:34-39`) + `ModelLoadResult` (`:41-54`): add `std::string assetKey;` to each.
(b) `EnqueueModelLoadJob` (`GameThread.cpp:677-688`): add a param `const std::string& assetKey` and set `job.assetKey = assetKey;`. Update its declaration in `GameThread.h`.
(c) Startup enqueue (`GameThread.cpp:127-140`): compute the logical key and pass it:
```cpp
                const std::string objPath = entry.path().string();
                const std::string mtlBaseDir = entry.path().parent_path().string();
                const std::string assetKey = NormalizeAssetKey(objPath); // e.g. "models/tree.obj"
                EnqueueModelLoadJob(m_NextLoadTicket.fetch_add(1, std::memory_order_relaxed),
                                    objPath, mtlBaseDir, assetKey);
```
Add `#include "AssetKey.h"` to `GameThread.cpp`.
(d) The worker copies `job.assetKey` into the `ModelLoadResult` it produces (find where the worker constructs the result in `WorkerThreadFunc` and set `result.assetKey = job.assetKey;`).
(e) Mesh-upload command build (`GameThread.cpp:361-365` area): copy the key into the fixed buffer:
```cpp
                        meshCmd.TicketId = res.ticketId;
                        std::snprintf(meshCmd.MeshRequest.Key, sizeof(meshCmd.MeshRequest.Key), "%s", res.assetKey.c_str());
```
(The response-drain at `:458` already does `m.MeshId = response.Mesh.Handle.Index;` — now a stable hash. `StoreMeshCpuData(response.Mesh.Handle.Index, …)` at `:445` is already uint64 from Task B. No change there.)

- [ ] **Step 8: Editor mesh picker (`MeshManagerPanel.cpp` + `MeshEditor.cpp`)**

`MeshManagerPanel.cpp:195` (file-load `AddMesh`): pass `NormalizeAssetKey(filePath)` as the first arg (the `filePath` is in scope from the dialog). Add `#include "AssetKey.h"`.
`MeshEditor.cpp` dropdown: replace the slot-index combo with a key-based one. Build options from `ctx.MeshSys->GetAssetList()`; show each `key`; on selection set `m_St.edit.MeshId = handle;`. Replace the body of the `if (ctx.MeshSys)` block:
```cpp
    if (ctx.MeshSys) {
        const auto assets = ctx.MeshSys->GetAssetList(); // vector<pair<uint64_t,string>>
        std::string current = ctx.MeshSys->KeyForHandle(m_St.edit.MeshId);
        if (current.empty()) current = "(none)";
        if (ImGui::BeginCombo("Mesh", current.c_str())) {
            for (const auto& [handle, key] : assets) {
                const bool isSelected = (handle == m_St.edit.MeshId);
                if (ImGui::Selectable(key.c_str(), isSelected)) { m_St.edit.MeshId = handle; m_St.modified = true; }
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (assets.empty()) ImGui::TextDisabled("No meshes loaded");
    } else {
        if (ImGui::InputScalar("Mesh handle", ImGuiDataType_U64, &m_St.edit.MeshId)) m_St.modified = true;
    }
```
`AddDefault` keeps `newMesh.MeshId = 0;` (kMissingAssetHandle / default mesh).

- [ ] **Step 9: Build full tree — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: clean. (Materials still slot-based; that's Task D.)

- [ ] **Step 10: Commit**
```
git -C C:/dev/clang-examples add -- src/engine/src/rendering/MeshSystem.h src/engine/src/rendering/MeshSystem.cpp src/engine/src/rendering/Renderer.h src/engine/src/rendering/Renderer.cpp src/common/include/ApplicationContext.h src/engine/src/threading/RenderThread.cpp src/engine/src/threading/GameThread.h src/engine/src/threading/GameThread.cpp src/editor/src/panels/MeshManagerPanel.cpp src/editor/src/panels/inspector/MeshEditor.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(asset): MeshSystem keyed by logical asset path (stable hash handle, de-dup, registry lookups, editor picker)"
```

---

### Task D: MaterialSystem keyed registry — end-to-end

**Files:** `MaterialSystem.{h,cpp}`, `Renderer.{h,cpp}`, `ApplicationContext.h` (MaterialRequest.Key), `RenderThread.cpp`, `GameThread.cpp`, `MaterialManagerPanel.cpp`, `MaterialEditor.cpp`.

Mirror Task C for materials. Material logical key = source texture path (editor file-load) or, for the async model-load material, the synthesized `"<modelKey>#mat0"` (the loader currently supports one material per model — `ModelLoadResult` comment).

- [ ] **Step 1: MaterialSystem keying (`MaterialSystem.{h,cpp}`)**

Add `std::string key;` to `MaterialEntry`; add `std::unordered_map<uint64_t,uint32_t> m_SlotByHandle;` + `int32_t SlotForHandle(uint64_t) const;` private; add public `std::string KeyForHandle(uint64_t) const;` and `std::vector<std::pair<uint64_t,std::string>> GetAssetList() const;`. Change `AddMaterial` signature to `AddMaterial(std::string key, const uint32_t* textureRgba8, uint32_t texWidth, uint32_t texHeight)`. Add `#include "AssetKey.h"`, `<unordered_map>`, `<string>`, `<utility>`.

In `AddMaterial`, at the top (after the `!m_Device` guard) add the de-dup + handle:
```cpp
    const uint64_t handle = AssetKeyHash(key);
    if (auto it = m_SlotByHandle.find(handle); it != m_SlotByHandle.end()) return MaterialHandle{ handle };
```
Set `entry.key = std::move(key);` in both branches (no-texture + texture). Replace BOTH `const uint32_t materialId = static_cast<uint32_t>(m_Materials.size()); m_Materials.push_back(entry); … return MaterialHandle{ materialId };` blocks with:
```cpp
    const uint32_t slot = static_cast<uint32_t>(m_Materials.size());
    m_Materials.push_back(std::move(entry));
    m_SlotByHandle[handle] = slot;
    return MaterialHandle{ handle };
```
Change failure returns `UINT32_MAX`→`UINT64_MAX`. Rewrite `GetMaterialResources(uint64_t materialId)` to resolve via `SlotForHandle` (unknown → warn + invalid resources; caller falls back to `MissingMaterial`). `GetMaterialCount()` stays `m_Materials.size()`. Implement `KeyForHandle`/`GetAssetList`/`SlotForHandle` exactly as for MeshSystem.
**Initialize** (`MaterialSystem.cpp:4-20`): the default material (slot 0, magenta checkerboard) must register under the empty key → `handle 0`:
```cpp
    MaterialEntry defaultMaterial{};
    defaultMaterial.key = "";
    defaultMaterial.texture = m_MissingMaterial;
    defaultMaterial.sampler = m_DefaultSampler;
    defaultMaterial.usesMissingTexture = true;
    m_SlotByHandle[kMissingAssetHandle] = 0;
    m_Materials.push_back(defaultMaterial);
```

- [ ] **Step 2: Renderer forwarder (`Renderer.{h,cpp}`)** — `AddMaterial(std::string key, const uint32_t* …)` forwarding `m_MaterialSystem.AddMaterial(std::move(key), …)`.

- [ ] **Step 3: Ring key (`ApplicationContext.h` MaterialRequest)** — add `char Key[256];` to the `MaterialRequest` union struct (after `Texture`).

- [ ] **Step 4: RenderThread (`RenderThread.cpp` RequestMaterial case)** — pass `std::string(cmd.MaterialRequest.Key)` as the first arg to `AddMaterial`; change `materialHandle.Index != UINT32_MAX` → `!= UINT64_MAX`.

- [ ] **Step 5: GameThread material key** — in the material-upload command build (`GameThread.cpp:412-420` area), set the synthesized key:
```cpp
                    materialCmd.TicketId = res.ticketId;
                    materialCmd.MaterialRequest.Width = res.Width;
                    materialCmd.MaterialRequest.Height = res.Height;
                    materialCmd.MaterialRequest.Texture = res.Texture;
                    const std::string matKey = res.assetKey + "#mat0";
                    std::snprintf(materialCmd.MaterialRequest.Key, sizeof(materialCmd.MaterialRequest.Key), "%s", matKey.c_str());
```
(The response-drain `m.MaterialId = response.Material.Handle.Index;` at `:471` already yields the stable hash.)

- [ ] **Step 6: Editor material picker (`MaterialManagerPanel.cpp` + `MaterialEditor.cpp`)** — `MaterialManagerPanel.cpp:86` file-load `AddMaterial`: pass `NormalizeAssetKey(filePath)` first (add `#include "AssetKey.h"`). `MaterialEditor.cpp` dropdown: same key-based combo as MeshEditor (build from `ctx.MatSys->GetAssetList()`, show `key`, assign `handle`; fallback `InputScalar(..., ImGuiDataType_U64, &m_St.edit.MaterialId)`). Keep the BaseColor/Flags editors unchanged.

- [ ] **Step 7: Build full tree — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community
```

- [ ] **Step 8: Commit**
```
git -C C:/dev/clang-examples add -- src/engine/src/rendering/MaterialSystem.h src/engine/src/rendering/MaterialSystem.cpp src/engine/src/rendering/Renderer.h src/engine/src/rendering/Renderer.cpp src/common/include/ApplicationContext.h src/engine/src/threading/RenderThread.cpp src/engine/src/threading/GameThread.cpp src/editor/src/panels/MaterialManagerPanel.cpp src/editor/src/panels/inspector/MaterialEditor.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(asset): MaterialSystem keyed by logical key (texture path / model#mat synth; stable hash handle, editor picker)"
```

---

### Task E: Key-based serialization + install the registry + round-trip tests

**Files:** `src/common/include/ComponentSerialization.h`, `src/engine/src/threading/RenderThread.cpp`, `tests/test_worldserial.cpp`.

- [ ] **Step 1: Write the failing round-trip test (`tests/test_worldserial.cpp`)**

At the top of the file add the registry include + a stub:
```cpp
#include "AssetRegistry.h"
#include "AssetKey.h"
```
Add a stub registry + test function after `T11_collider_backward_compatible_defaults()`:
```cpp
static std::string StubMeshKey(uint64_t h) { return h == AssetKeyHash("models/x.obj") ? "models/x.obj" : std::string(); }
static std::string StubMatKey(uint64_t h)  { return h == AssetKeyHash("textures/y.png") ? "textures/y.png" : std::string(); }

static void T12_asset_key_roundtrip()
{
    static const AssetRegistry stub{ &StubMeshKey, &StubMatKey };
    SetAssetRegistry(&stub);

    MeshComponent mIn; mIn.MeshId = AssetKeyHash("models/x.obj"); mIn.Visible = true;
    const nlohmann::json jm = mIn;
    EXPECT(jm.at("MeshKey").get<std::string>() == "models/x.obj");
    const auto mOut = jm.get<MeshComponent>();
    EXPECT(mOut.MeshId == AssetKeyHash("models/x.obj"));
    EXPECT(mOut.Visible == true);

    MaterialComponent matIn; matIn.MaterialId = AssetKeyHash("textures/y.png");
    matIn.BaseColor = glm::vec4(0.2f,0.4f,0.6f,1.0f); matIn.Flags = 1u;
    const nlohmann::json jt = matIn;
    EXPECT(jt.at("MaterialKey").get<std::string>() == "textures/y.png");
    const auto matOut = jt.get<MaterialComponent>();
    EXPECT(matOut.MaterialId == AssetKeyHash("textures/y.png"));
    EXPECT(matOut.Flags == 1u);

    // Empty key -> Missing handle.
    nlohmann::json jEmpty = { {"MeshKey", ""}, {"Visible", false} };
    EXPECT(jEmpty.get<MeshComponent>().MeshId == kMissingAssetHandle);

    SetAssetRegistry(nullptr); // don't leak the stub to other tests
}
```
Register `T12_asset_key_roundtrip();` in `main()` after the T11 call.

- [ ] **Step 2: Build the test — confirm it FAILS**
```
cmake --build --preset msvc-win64-vs2026-community --target test_worldserial
```
Expected: FAIL — `to_json(MeshComponent)` still writes `"MeshId"` (numeric), so `jm.at("MeshKey")` throws / assertion fails. (TDD red.)

- [ ] **Step 3: Switch the serializers to keys (`ComponentSerialization.h`)**

Add includes at the top: `#include "AssetKey.h"` and `#include "AssetRegistry.h"`. Replace the `MeshComponent` (de)serializers (`:26-32`):
```cpp
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
```
Replace the `MaterialComponent` (de)serializers (`:34-41`):
```cpp
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
```
(Confirm `ComponentSerialization.h` includes the header providing `SM_WARN` — it includes `lib.h` transitively via `ECS.h`/others; if not, add `#include "lib.h"`.)

- [ ] **Step 4: Build + run the test — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community --target ecs --target test_worldserial
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
```
Expected: `All world-serialization tests passed.`

- [ ] **Step 5: Install the AssetRegistry from Engine (`RenderThread.cpp`)**

Near the top of `RenderThread.cpp` add `#include "AssetRegistry.h"` and file-static glue:
```cpp
namespace {
    MeshSystem*     g_MeshSysForRegistry = nullptr;
    MaterialSystem* g_MatSysForRegistry  = nullptr;
    std::string MeshKeyForHandleImpl(uint64_t h)     { return g_MeshSysForRegistry ? g_MeshSysForRegistry->KeyForHandle(h) : std::string(); }
    std::string MaterialKeyForHandleImpl(uint64_t h) { return g_MatSysForRegistry  ? g_MatSysForRegistry->KeyForHandle(h)  : std::string(); }
    const AssetRegistry kAssetRegistry{ &MeshKeyForHandleImpl, &MaterialKeyForHandleImpl };
}
```
In `RenderThread::Initialize()` (`:231-244`), after `m_Renderer->Init(...)` succeeds, wire + install:
```cpp
    g_MeshSysForRegistry = m_Renderer->GetMeshSystem();
    g_MatSysForRegistry  = m_Renderer->GetMaterialSystem();
    SetAssetRegistry(&kAssetRegistry);
```
(Include `MeshSystem.h`/`MaterialSystem.h` if not already included in `RenderThread.cpp`.)

- [ ] **Step 6: Build full tree — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community
```

- [ ] **Step 7: Commit**
```
git -C C:/dev/clang-examples add -- src/common/include/ComponentSerialization.h src/engine/src/threading/RenderThread.cpp tests/test_worldserial.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(asset): world.json stores logical keys via AssetRegistry seam; install registry; round-trip test"
```

---

### Task F: Full regression + manual smoke

**Files:** none (verification; commit fixups only if needed).

- [ ] **Step 1: Reconfigure + full clean build**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community
```
Expected: all targets (ecs, Engine, editor, game, runtime, all test_*) build, no errors / `LNK`.

- [ ] **Step 2: Run the suites**
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_assetkey.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_compserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_reloadpreserve.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_playermove.exe
```
Expected: each prints its pass line. (`test_compserial` emits the expected `NopeNotReal` warn; `test_reloadpreserve` the expected `BadComp` warn.)

- [ ] **Step 3: Manual smoke (human-owned, after restarting the editor — `ECS.h`/`ApplicationContext.h` changed)**
1. **Readable saves:** author a scene with a model + texture, Save → open `world.json`, confirm entities show `"MeshKey": "models/…"` / `"MaterialKey": "…"` (not numbers).
2. **The bug, fixed:** add a NEW model that sorts alphabetically BEFORE an existing one into `assets/models/`, restart → existing entities still show the CORRECT mesh/material (pre-fix this would shift).
3. **Missing asset:** hand-edit a `MeshKey`/`MaterialKey` to a non-existent path (or remove the file), load → entity renders Missing mesh / magenta-checkerboard material + `SM_WARN`.
4. **Pickers:** the inspector Mesh/Material dropdowns list assets by readable key; selecting one assigns correctly and renders.
5. **Duplicate:** Ctrl+D a model entity → the copy keeps the same mesh/material.

- [ ] **Step 4: Commit fixups (only if needed).**

---

## Done criteria

- `AssetKey.h` (pure hash + normalize) and `AssetRegistry.h` (seam) exist; `test_assetkey` green (Task A).
- All mesh/material handles + component ids are `uint64`; tree builds, suites green with no behavior change (Task B).
- `MeshSystem`/`MaterialSystem` are keyed registries (de-dup, `hash→slot`, `KeyForHandle`, `GetAssetList`); `AddMesh`/`AddMaterial` take the logical key; render/nav/picking callers unchanged (Tasks C, D).
- The async load path threads the logical key end-to-end; editor pickers list by key (Tasks C, D).
- `world.json` persists `MeshKey`/`MaterialKey`; load uses pure `AssetKeyHash`; save uses the installed `AssetRegistry`; `test_worldserial` T12 round-trip green (Task E).
- Adding/reordering assets no longer corrupts saved references (manual smoke b); unresolved → Missing/magenta + `SM_WARN`.
- Full tree builds; all six suites green. No `GAME_API_VERSION` bump; rebuild ecs+Engine+editor+game + editor restart documented.

## Notes

- The expensive-to-retrofit decisions (logical keys in `world.json`, the `AssetRegistry` seam) are made now; the cheap-to-change representation (uint64 hash handle) stays swappable behind the seam. A future VFS re-backs `AssetRegistry` + the `AddMesh`/`AddMaterial` byte source; if it introduces GUIDs, the seam maps key→GUID with a one-time mechanical `world.json` migration. See the spec's Future-VFS section + [[project_id_stability]].
- Material key for the async path is `"<modelKey>#mat0"` because the loader currently supports one material per model (`ModelLoadResult`); when multi-material lands, extend to `#matN`.
- `SubMesh.MaterialIndex` is runtime-only (mesh-internal, not persisted) but widened to carry a material handle.
- Entity-id instability (sibling gotcha in [[project_id_stability]]) is out of scope.
