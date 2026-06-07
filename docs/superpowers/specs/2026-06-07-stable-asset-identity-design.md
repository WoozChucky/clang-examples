# Stable asset identity (mesh + material) — Design

**Date:** 2026-06-07
**Branch:** `feat/asset-id-stability`
**Status:** Approved (brainstorm) — ready for implementation plan

## Problem

`MeshComponent.MeshId` and `MaterialComponent.MaterialId` (`ECS.h:60,71`) are raw `uint32_t`
**slot indices** into `MeshSystem::m_Meshes` / `MaterialSystem::m_Materials` (vectors,
`MeshSystem.h:93` / `MaterialSystem.h:69`), assigned by `AddMesh`/`AddMaterial` push-back order.
At startup every model is loaded via `std::filesystem::directory_iterator("assets/models")`
(`GameThread.cpp:130`), so the id is the directory-iteration position. `world.json` persists that
bare index (`ComponentSerialization.h:27,35`). Adding / renaming / removing an asset shifts the
indices of everything after it, so a saved scene's `MeshId`/`MaterialId` then points at the **wrong**
asset. This is a positional-index-as-persistent-id bug. See [[project_id_stability]].

A future asset packer/unpacker (a virtual filesystem the shipped game reads at runtime) is planned.
The design must not bake assumptions that force a full refactor when that lands.

## Goal

- Persisted asset references survive changes to the asset set (add/remove/reorder/rename-other).
- The persisted identity is a **stable logical key** (a virtual path under `assets/`) — the same
  addressing a VFS uses — so `world.json` never needs migrating when the packer arrives.
- A single **resolution seam** (`AssetRegistry`) maps logical key ↔ runtime handle; the future VFS
  re-backs that one interface and nothing above it moves.
- Minimal blast radius on the many render/nav/picking consumers (they stay numeric).
- Editor asset pickers list assets by readable key, not `"Mesh 3"`.

## Non-goals

- No asset-import pipeline, `.meta` sidecars, or GUIDs now (GUIDs are a VFS-era upgrade absorbed by
  the seam — see "Future VFS").
- No backward-compat migration of old `world.json` numeric ids (early-dev breaking is acceptable —
  see [[early-dev-breaking-ok]]); unresolved refs fall back to Missing + `SM_WARN`.
- No change to how meshes/materials are *uploaded* to the GPU (still NVRHI via the ring), only how
  they are *identified and addressed*.
- Sub-mesh→material association (`AssociateMeshMaterial`) stays runtime-only (not persisted); it just
  uses the new handle space.

## Identity model

### Logical key
A `std::string` virtual path under `assets/`, forward-slashed, e.g. `models/tree.obj`,
`textures/bark.png`. Treated as an **opaque stable identifier** (today it equals a virtual path; the
VFS may later map it to a GUID). This is what `world.json` stores.

### Runtime handle = hash of the key
`MeshHandle.Index` / `MaterialHandle.Index` and `MeshComponent.MeshId` /
`MaterialComponent.MaterialId` become a **64-bit FNV-1a hash of the logical key**. Properties:
- **Stable across runs** (derived purely from the key, independent of load order).
- **Numeric** → every existing consumer that reads the id and calls
  `meshSystem->GetMeshResources(id)` / `GetMeshBounds` / `IsValidMeshId` / `GetMeshCpuData` /
  `GetMaterialResources` is **unchanged** (the system maps hash→slot internally).
- **Async-safe** — derivable from the key the instant a world loads, before the asset is uploaded;
  the system returns Missing until the slot exists, then resolves automatically.

The width change `uint32_t → uint64_t` is a breaking layout change (acceptable; early-dev).

### Hashing — shared pure function
New `src/common/include/AssetKey.h`:
```cpp
#pragma once
#include <cstdint>
#include <string_view>

// 64-bit FNV-1a over the logical asset key bytes. Pure + header-only so every module
// (ecs.dll, Engine, editor, game, tests) derives the SAME handle from the SAME key with no
// shared state. Reserved handle 0 == the Missing/default asset (no key hashes to 0 in practice;
// the empty key is mapped to 0 explicitly by callers/registry).
inline uint64_t AssetKeyHash(std::string_view key) {
    uint64_t h = 1469598103934665603ull;            // FNV offset basis
    for (unsigned char c : key) { h ^= c; h *= 1099511628211ull; } // FNV prime
    return h;
}

inline constexpr uint64_t kMissingAssetHandle = 0ull; // Missing mesh / magenta-checkerboard material
```
The empty key (`""`) and any unresolved handle map to `kMissingAssetHandle` (0).

## Components

### 1. `MeshSystem` / `MaterialSystem` become keyed registries

- Add `std::string key;` to `MeshEntry` (`MeshSystem.h:78`) and `MaterialEntry`
  (`MaterialSystem.h:54`).
- `AddMesh(std::string key, const MeshVertex* …)` / `AddMaterial(std::string key, const uint32_t* …)`:
  - If `key` is already registered, return the existing handle (**de-dup** — also removes redundant
    re-uploads of the same asset).
  - Else create the slot, store `key`, populate two maps: `m_SlotByHandle[AssetKeyHash(key)] = slot`
    and (implicitly via the entry) handle→key. Return `MeshHandle{ Index = AssetKeyHash(key) }`.
- Existing id-keyed lookups take the handle (hash) and map `handle → slot` via `m_SlotByHandle`
  (vector remains the storage). Unknown handle → Missing slot (0).
- Reserved slot 0 keeps the Missing/default entry; its handle is `kMissingAssetHandle` (0).
  Material slot 0 is the **magenta-checkerboard missing texture** (`MaterialSystem.cpp:11-17`,
  `usesMissingTexture=true`) — so any unresolved material renders as the checkerboard (visually
  obvious). Mesh slot 0 is `MissingMesh`.
- New enumeration for editor pickers + the registry bridge:
  `std::vector<std::pair<uint64_t,std::string>> MeshSystem::GetAssetList() const;` (handle + key for
  every loaded mesh), same for `MaterialSystem`.
- New reverse lookup: `std::string MeshSystem::KeyForHandle(uint64_t) const;` (empty if unknown),
  same for materials. (Forward lookup is just `AssetKeyHash(key)` — pure, no system call.)
- `DestroyGpuResources`/`RecreateGpuResources` preserve `key` + the `m_SlotByHandle` map (they
  already preserve slots/CPU caches).

### 2. The seam — `AssetRegistry` bridge

New `src/common/include/AssetRegistry.h` — a fn-ptr struct (same idiom as `NavServices`/`EditorUI`),
populated by Engine from the two systems, callable from `common/` without depending on Engine:
```cpp
#pragma once
#include <cstdint>
#include <string>

// Resolution seam between a stable logical asset key and the runtime handle. Today backed by
// MeshSystem/MaterialSystem; the future VFS re-backs THIS interface (key->bytes/handle) and
// nothing above it changes. Forward resolution (key->handle) is pure AssetKeyHash and does not
// need the registry; the registry provides the REVERSE map (handle->key) for serialization +
// editor display, which only the owning systems know.
struct AssetRegistry {
    std::string (*MeshKeyForHandle)(uint64_t handle);      // "" if unknown
    std::string (*MaterialKeyForHandle)(uint64_t handle);  // "" if unknown
};

// Process-wide pointer, set by Engine at init (RenderThread owns the systems). Null in contexts
// with no asset systems (e.g. unit tests) — callers fall back gracefully. Defined in ecs.dll so
// common-header serializers can reach it.
ECS_API void SetAssetRegistry(const AssetRegistry* reg);
ECS_API const AssetRegistry* GetAssetRegistry();
```
- Set once by Engine after `MeshSystem`/`MaterialSystem` exist (RenderThread side).
- `MeshKeyForHandle`/`MaterialKeyForHandle` are thin wrappers over `MeshSystem::KeyForHandle` etc.
- Thread note: the only consumer of the *reverse* map is **save** (editor `MainMenuBar` → WorldManager
  save, on the RenderThread where the systems live) → same-thread, safe. **Load** needs only
  `AssetKeyHash(key)` (pure) → no registry access, no thread coupling, async-safe.

### 3. Serialization — `world.json` stores the key

`ComponentSerialization.h` `MeshComponent`/`MaterialComponent` (de)serializers switch from the raw
index to the logical key:
```cpp
inline void to_json(nlohmann::json& j, const MeshComponent& t) {
    std::string key;
    if (const AssetRegistry* r = GetAssetRegistry()) key = r->MeshKeyForHandle(t.MeshId);
    j = nlohmann::json{{"MeshKey", key}, {"Visible", t.Visible}};
}
inline void from_json(const nlohmann::json& j, MeshComponent& t) {
    std::string key; j.at("MeshKey").get_to(key);
    t.MeshId = key.empty() ? kMissingAssetHandle : AssetKeyHash(key);
    j.at("Visible").get_to(t.Visible);
}
```
(Material analogous: `"MaterialKey"` + the existing `"BaseColor"`/`"Flags"`.) The `world.json`
per-entity block now reads e.g. `"MeshComponent": { "MeshKey": "models/tree.obj", "Visible": true }`.
- Save with no registry installed (or an unregistered handle) → empty key + `SM_WARN`
  (`"MeshComponent on entity has unresolved handle %llu — saved as empty"`), never silent.
- Load never needs the registry (pure hash); a key whose asset isn't present resolves to a handle
  that the system reports Missing for at render time → Missing visual + `SM_WARN` there.

### 4. Async key plumbing

The source path is currently discarded before `AddMesh` (the ring carries pixels + ticket only). Add
the key end-to-end:
- `ModelLoadJob` already has `objPath`/`mtlBaseDir` (`GameThread.cpp:679-682`). Carry a derived
  **logical key** (the virtual path, e.g. `models/tree.obj` — relativized from `assets/`) into
  `ModelLoadResult`.
- Add a `std::string Key` (mesh) and per-material key to `RendererCommand`'s `MeshRequest` /
  `MaterialRequest` (`ApplicationContext.h:93-106`). (These structs already cross the ring; adding a
  string member is consistent with `ECSCommand` carrying strings.)
- `RenderThread.cpp:106/135` pass the key into `AddMesh(key,…)` / `AddMaterial(key,…)`.
- The `RendererResponse` already returns `Handle.Index`; with hashed handles that *is* the stable
  id, so the existing wiring at `GameThread.cpp:458/471` (`MeshId = response…Handle.Index`) keeps
  working unchanged, now writing a stable hash.
- Editor file-load sites (`MeshManagerPanel.cpp:195`, `MaterialManagerPanel.cpp:86`) already have the
  path in scope → relativize to a logical key and pass it in.

### 5. Material logical key scheme

- Material with a source **texture file** → key = the texture's virtual path
  (e.g. `textures/bark.png`). Standalone editor-loaded textures (`MaterialManagerPanel`) use their
  file path directly.
- Material from a model with **no standalone texture** → synthesized stable key
  `"<modelKey>#mat<index>"` (e.g. `models/tree.obj#mat0`) — stable across reloads of that model.
- **Default / textureless** material (slot 0) → reserved empty key `""` → `kMissingAssetHandle`
  (magenta checkerboard).
- De-dup by key means a texture shared across models registers once.

### 6. Editor pickers

- `MeshEditor.cpp` / `MaterialEditor.cpp` dropdowns: build options from
  `MeshSystem::GetAssetList()` / `MaterialSystem::GetAssetList()` showing the **key** (readable),
  and on selection set the component id to that entry's handle (the hash). Replace the
  `"Mesh "+i` / `"Material "+i` slot labels and the raw `InputScalar` fallback.
- `MeshManagerPanel` / `MaterialManagerPanel` lists show the key alongside the preview.

## Data flow

**Load (startup / world load):** `world.json` "MeshKey" string → `from_json` sets
`MeshId = AssetKeyHash(key)` (pure). Meanwhile models stream in async; each `AddMesh(key,…)`
registers `hash(key)→slot`. When a render pass reads the entity's `MeshId` and calls
`GetMeshResources(MeshId)`, the system maps the hash→slot if loaded, else returns Missing. No
ordering dependency between world-load and asset-load.

**Save (editor):** WorldManager iterates entities → `to_json(MeshComponent)` calls
`GetAssetRegistry()->MeshKeyForHandle(MeshId)` (RenderThread; systems present) → writes the readable
key.

**Runtime resolution:** handle (hash) → `m_SlotByHandle` → vector slot → GPU resources. Same number
flows through NavMesh cache (`StoreMeshCpuData` keyed by `Handle.Index`) and the GBuffer batch key —
all transparently, since they already carry `Handle.Index`.

## Error handling / edge cases

- **Asset key not loaded at render** → Missing mesh / magenta-checkerboard material + `SM_WARN`
  (once per unresolved handle per frame is noisy → warn-once via a small seen-set, or warn at
  resolution failure with throttling; plan picks a non-spammy form).
- **Save with no registry / unresolved handle** → empty key persisted + `SM_WARN`.
- **Hash collision** (two distinct keys → same 64-bit hash) → astronomically unlikely; `AddMesh`
  `SM_ERROR`s if it registers a handle already mapped to a *different* key (detection, not silent
  shadowing).
- **Renaming an asset file** → its key changes → old `world.json` refs become Missing (correct: it's
  a different logical asset now); warned, re-author. (GUIDs would survive renames — deferred.)
- **Old `world.json` with numeric `MeshId`/`MaterialId`** → `from_json` reads `"MeshKey"` (absent) →
  `j.at` throws → the registry `load` is wrapped (existing `ModifyComponentJson`/load paths catch),
  or we accept the breaking format change and the entity loads unnamed/Missing. Early-dev: breaking
  is fine; no shim.

## Migration

Breaking `world.json` format change (`MeshId`→`MeshKey`, `MaterialId`→`MaterialKey`). No shim.
Re-save scenes once after the change. Consistent with [[early-dev-breaking-ok]].

## Testing

- **`AssetKey.h`**: unit test (extend `test_worldserial` or a tiny new `test_assetkey`) — determinism
  (`AssetKeyHash("models/tree.obj")` constant), distinct keys → distinct hashes for a sample set,
  empty key handling.
- **Serialization round-trip** (`test_worldserial`): with a **stub `AssetRegistry`** installed
  (mapping a known handle↔key), `MeshComponent{MeshId=hash("models/x.obj"),Visible=true}` →
  `to_json` writes `"MeshKey":"models/x.obj"` → `from_json` recovers `MeshId == hash(...)`. Material
  analogous. Also: `from_json` of an empty key → `kMissingAssetHandle`.
- **Registry de-dup**: `AddMesh("k",…)` twice returns the same handle; `KeyForHandle(h)` round-trips.
  (Unit-testable at the system level if a headless MeshSystem path exists; else build-verified +
  manual.)
- **Full regression**: `test_ecs`, `test_worldserial`, `test_compserial`, `test_reloadpreserve`,
  `test_playermove` green; full tree builds.
- **Manual smoke (human-owned):** (a) author a scene referencing a model + texture, save → inspect
  `world.json` shows readable keys; (b) **add a new model that sorts earlier alphabetically**, restart
  → existing entities still show the correct mesh/material (the original bug, now fixed); (c) reference
  a missing asset → magenta checkerboard + warn; (d) editor dropdowns list assets by name; selecting
  one assigns correctly; (e) duplicate (Ctrl+D) preserves the asset refs.

## Future VFS

The packer/VFS becomes **a new `AssetRegistry` backend + a new `AddMesh`/`AddMaterial` byte source**:
`world.json` already stores logical keys (the VFS's addressing), resolution already goes through the
seam, and the runtime handle (hash of the key) is unaffected by where the bytes come from. If the VFS
introduces GUIDs, the seam maps key→GUID (import step) and a one-time mechanical `world.json` key
migration runs — no change to components, consumers, or the resolution model.

## Affected files (from the consumer map)

| File | Change |
|------|--------|
| `src/common/include/AssetKey.h` | **new** — `AssetKeyHash` + `kMissingAssetHandle` |
| `src/common/include/AssetRegistry.h` | **new** — `AssetRegistry` bridge + `Set/GetAssetRegistry` |
| `src/ecs/src/*` (or a small new TU) | define the `Set/GetAssetRegistry` global (exported `ECS_API`) |
| `src/common/include/ECS.h` | `MeshComponent.MeshId` / `MaterialComponent.MaterialId` → `uint64_t` |
| `src/common/include/ComponentSerialization.h` | Mesh/Material (de)serializers → key via registry |
| `src/common/include/ApplicationContext.h` | `MeshHandle`/`MaterialHandle.Index`→u64; `MeshRequest`/`MaterialRequest` + `RendererResponse` carry the key |
| `src/engine/src/rendering/MeshSystem.{h,cpp}` | `key` field, handle=hash, `m_SlotByHandle`, `AddMesh(key,…)`, `KeyForHandle`, `GetAssetList`, id-lookups map hash→slot |
| `src/engine/src/rendering/MaterialSystem.{h,cpp}` | same as MeshSystem; slot 0 magenta-checkerboard = Missing |
| `src/engine/src/rendering/Renderer.{h,cpp}` | `AddMesh`/`AddMaterial` forwarders gain the key param |
| `src/engine/src/threading/RenderThread.cpp` | pass key into `AddMesh`/`AddMaterial`; install `AssetRegistry` |
| `src/engine/src/threading/GameThread.cpp` | derive logical key from `objPath`; thread it through `ModelLoadJob`→`ModelLoadResult`→`MeshRequest`/`MaterialRequest`; material key synth |
| `src/engine/src/navigation/NavMeshSystem.{h,cpp}` | cache key type u64 (handle space unchanged otherwise) |
| render passes (`GBufferFillPass`/`ShadowDepthPass`/`OutlineRenderPass`/`DebugRenderPass`), `ViewportPicker`, `ImGuiRenderer` | id type u64; **logic unchanged** (still pass the component id to the system) |
| `src/editor/src/panels/inspector/MeshEditor.cpp` / `MaterialEditor.cpp` | dropdowns list by key, assign hash |
| `src/editor/src/panels/MeshManagerPanel.cpp` / `MaterialManagerPanel.cpp` | pass key into file-load `AddMesh`/`AddMaterial`; show keys |
| `tests/test_worldserial.cpp` (+ maybe `tests/test_assetkey.cpp`) | hash + key round-trip with a stub registry |

## Done criteria

- `MeshComponent`/`MaterialComponent` persist a **logical key** in `world.json`; the runtime handle is
  `hash(key)` (uint64); adding/reordering assets no longer corrupts saved references (the original
  bug, verified by manual smoke (b)).
- `MeshSystem`/`MaterialSystem` are keyed registries (de-dup, hash→slot, reverse `KeyForHandle`,
  `GetAssetList`); id-based lookups unchanged for callers.
- The `AssetRegistry` seam exists and is the sole key↔handle reverse indirection; serialization uses
  it; load uses pure `AssetKeyHash`.
- Unresolved asset → magenta-checkerboard / MissingMesh + `SM_WARN`.
- Editor pickers list assets by readable key.
- Full tree builds; the five suites green; new hash + round-trip tests green.
- Rebuild scope: `ECS.h` + `ApplicationContext.h` change ⇒ rebuild **ecs + Engine + editor + game**,
  restart editor. No `GAME_API_VERSION` bump unless `GameState` layout changes (it doesn't).

## Notes

- Pattern reuses the codebase's fn-ptr service-bridge idiom (`NavServices`/`EditorUI`) for the seam,
  and the COW-snapshot / ring infrastructure unchanged.
- The expensive-to-retrofit decisions (logical keys in `world.json`, the resolution seam) are made
  now; the cheap-to-change representation (hash handle) stays swappable behind the seam — see
  [[project_id_stability]] and the Future VFS section.
- Entity-id instability (the sibling gotcha in [[project_id_stability]]) is **out of scope** here.
