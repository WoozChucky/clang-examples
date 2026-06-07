# Animation SP1 — Skeleton import + visualization — Design

**Date:** 2026-06-07
**Branch:** `feat/anim-skeleton-import`
**Status:** Approved (brainstorm) — ready for implementation plan

## Context

This is **sub-project 1 of the animation effort** (decomposition: SP1 import+viz → SP2 static
bind-pose skinning → SP3 single-clip playback → SP4+ blending/state-machine/IK). Each sub-project is
its own spec → plan → implementation cycle. SP1 de-risks the genuinely-unknown part (assimp skeleton
extraction) and produces a visible result with **zero** vertex-format / shader / PSO / skinning risk.

The engine has **no animation today**: `MeshVertex` is position/normal/uv only; the assimp load path
(`GameThread::WorkerThreadFunc`, ~`GameThread.cpp:727`) reads static meshes (`aiProcess_Triangulate |
GenSmoothNormals | JoinIdenticalVertices`) and discards bones, the node hierarchy, and any animation
data. Models load async over `assets/models` via `EnqueueModelLoadJob` → worker (assimp) →
`ModelLoadResult` → GameThread drains, posts `RendererCommand{RequestMesh}` to the GR ring, and on the
response sets `MeshComponent.MeshId`. Debug lines are drawn render-side: `DebugDraw.h`
(`DebugAppendLine`/`Box`/`Sphere`/…) writes into a `std::vector<DebugVertex>` consumed by
`DebugRenderPass`, which already does ShowGrid / ShowNavMesh / selected-entity AABB gated by debug
flags. The asset-identity work (just merged) gives a keyed `AssetRegistry` + `AssetKeyHash` (FNV-1a64)
+ the `#matN`-style synth-key convention — SP1 reuses it for skeleton keys.

OBJ has no skeletons, so SP1 introduces a **rigged glTF** test asset. (The startup loop already
accepts `.gltf`; assimp reads glTF.)

## Goal

Load a rigged glTF, extract its skeleton (bones + hierarchy + inverse-bind matrices) into a keyed,
immutable CPU asset, and draw the **bind-pose** skeleton as debug lines for any entity carrying a
`SkeletonComponent`, gated by a `ShowSkeleton` debug flag.

## Non-goals (all later sub-projects)

- No `SkinnedMeshVertex`, vertex-layout change, skinning shader, or G-buffer PSO variant (SP2).
- No `AnimationClip`, keyframe sampling, `AnimationComponent`, or bone-matrix palette / palette
  transport (SP2/SP3).
- No mesh deformation — the rigged mesh still renders **unskinned** via the existing path; only the
  debug skeleton shows the rig.
- No blending / state machine / IK / root-motion (SP4+).
- No `GAME_API_VERSION` bump (`GameState` layout unchanged).

## Components

### 1. Skeleton asset (immutable CPU data)
New `src/common/include/Skeleton.h` (or under engine — see file structure in the plan):
```cpp
struct Bone {
    std::string name;
    int         parent = -1;      // index into Skeleton::bones; -1 = root
    glm::mat4   localBind{1.0f};  // bone's local bind transform (rest pose), relative to parent
    glm::mat4   inverseBind{1.0f};// aiBone::mOffsetMatrix (mesh space -> bone space at bind)
};
struct Skeleton {
    std::vector<Bone> bones;      // topologically ordered so parent index < child index
};
```
`inverseBind` is unused for SP1 drawing (it's for SP2 skinning) but extracted now so the asset is
complete and SP2 needs no re-extraction. `localBind` drives the bind-pose joint positions SP1 draws.

### 2. `SkeletonStore` (engine, handle-keyed, immutable)
A registry mapping `uint64 handle -> Skeleton` (handle = `AssetKeyHash("<modelKey>#skeleton")`),
append-only, entries never mutated after insert. Mirrors the existing immutable-asset stores. Lives
engine-side so both GameThread (SP3 sampling, later) and RenderThread (SP1 viz) can read it by handle
with no lock (safe because an entry is inserted before its handle ever appears on an entity in a
published snapshot, and entries are immutable). Minimal API:
```cpp
uint64       Add(std::string key, Skeleton skeleton); // returns handle; de-dup by key
const Skeleton* Get(uint64 handle) const;             // nullptr if unknown
std::string  KeyForHandle(uint64 handle) const;       // for inspector/debug
```
(Same hashing/de-dup shape as MeshSystem/MaterialSystem from the asset-identity work.)

### 3. `SkeletonComponent` (engine builtin)
```cpp
struct SkeletonComponent { uint64_t SkeletonId = 0; }; // 0 = none
```
Engine builtin (X-macro + serializer registration), parallel to `MeshComponent` — it binds an entity
to an engine asset; the engine's load path attaches it and `DebugRenderPass` reads it. **Must** be
persisted (world.json entities reference already-loaded assets by handle — the startup directory scan
loads assets; per-entity loads are not re-issued on world load, same as `MeshComponent`). For SP1,
persist the **raw `uint64 SkeletonId`** directly (`to_json`/`from_json` read/write the number). This is
correct because the handle is a *stable hash* of the skeleton key (`AssetKeyHash("<modelKey>#skeleton")`)
— deterministic across runs, so the persisted number resolves to the same store entry next load (the
hash-handle is effectively the key, pre-hashed; unlike the old mesh *slot index*, it is stable). No
`AssetRegistry`-seam extension needed (which `common/` serialization couldn't reach for skeletons
anyway). SP2 may switch to key-string persistence via an extended seam if readable `world.json` is
wanted; out of scope for SP1. Attached at model-load when the glTF has a skeleton, alongside the
existing `MeshComponent`.

### 4. Import extraction (`GameThread::WorkerThreadFunc`)
When the loaded `aiScene` has bones: build the `Skeleton`.
- Collect every bone referenced by `aiMesh::mBones` across the scene's meshes: `mName`,
  `mOffsetMatrix` (→ `inverseBind`).
- Resolve the **hierarchy + local bind** from the `aiNode` tree: for each bone-named node, its parent
  is the nearest ancestor node that is also a bone; `localBind` = that node's `mTransformation`.
  Topologically order bones so `parent < child`.
- Convert assimp's row-major `aiMatrix4x4` to glm (transpose), consistent with however the existing
  mesh transforms are handled (verify in the loader; match it).
- Stash the `Skeleton` + its logical key (`NormalizeAssetKey(objPath) + "#skeleton"`) into
  `ModelLoadResult` (new fields). **Not** sent over the GR ring — skeleton is CPU data consumed on
  GameThread (registration) and read render-side from the store (viz).

### 5. GameThread registration
In the completed-jobs / response drain: if `res` has a skeleton, `SkeletonStore::Add(key, skeleton)`
→ handle, and attach `SkeletonComponent{ handle }` to the loaded entity (next to `MeshComponent`). For
startup non-entity ticket loads (no entity), still register the skeleton in the store (so it exists by
handle) — same as meshes land in MeshSystem without an entity.

### 6. Visualization (`DebugRenderPass`)
Add a `ShowSkeleton` debug flag (mirror `ShowGrid`/`ShowNavMesh`; editor sets it like the others). When
set, for each entity with `SkeletonComponent` + `TransformComponent` in the snapshot:
- `const Skeleton* sk = SkeletonStore.Get(comp.SkeletonId);` (skip + nothing if null).
- Compute bind-pose global per bone: `global[b] = (bones[b].parent < 0) ? localBind[b] :
  global[parent] * localBind[b]` (single forward pass; topological order guarantees parent computed
  first).
- Joint world position = `entityWorld * global[b] * vec4(0,0,0,1)`.
- For each non-root bone, `DebugAppendLine(jointPos[parent], jointPos[b], color)`.

## Data flow

rigged `.gltf` in `assets/models` → worker (assimp) extracts `Skeleton` + key → `ModelLoadResult`
(CPU, no ring) → GameThread drain: `SkeletonStore::Add` + attach `SkeletonComponent` → snapshot
published → RenderThread `DebugRenderPass` (if `ShowSkeleton`) reads the immutable store by handle and
appends bind-pose bone lines. Mesh still uploads + renders via the existing static path unchanged.

## Error handling / edge cases

- **glTF with no skeleton** (or an OBJ) → no bones extracted → no `SkeletonComponent` attached → mesh
  loads exactly as today. No error.
- **`SkeletonComponent` with an unknown/stale handle** → `SkeletonStore.Get` returns null → viz skips
  it; `SM_WARN` once (consistent with the asset-Missing handling).
- **Bone references a node not found / cyclic** → extraction logs `SM_WARN` and skips that skeleton
  (mesh still loads). Topological ordering assumes a tree (glTF skeletons are trees).
- **Matrix convention mismatch** (assimp row-major vs glm column-major) → bones draw in the wrong
  place; caught by the manual smoke against `RiggedSimple` and the unit test on synthetic data.
- **`ShowSkeleton` off (default, and runtime)** → zero cost; viz never runs.

## Testing

- **Unit (`tests/test_skeleton.cpp`, new):** the bind-pose hierarchy math — build a synthetic 2-bone
  `Skeleton` (root at origin, child offset by a known local translation), run the `global = parent *
  local` forward pass, assert the child joint world position equals the expected value. Also assert
  topological invariant (`parent < child` index). Pure math, no assimp/GPU.
- **Asset:** add `assets/models/RiggedSimple.gltf` (+ its `.bin`/textures) from the Khronos
  glTF-Sample-Assets repo. (Gitignored-assets caveat: this is a committed test asset under
  `assets/models`, distinct from the local `world.json`/settings files — confirm it's not caught by a
  broad ignore; commit it explicitly.)
- **Build-verified + manual smoke (human-owned):** load with `RiggedSimple` present, toggle
  `ShowSkeleton` → two bones draw at the correct bind-pose positions, moving/rotating the entity moves
  the skeleton with it. glTF without a rig → no skeleton, no crash. Old `world.json` without
  `SkeletonComponent` → loads fine.

## Done criteria

- A rigged glTF loads; its `Skeleton` (bones, parents, localBind, inverseBind) is extracted and stored
  in `SkeletonStore` by `#skeleton` key; the entity gets a `SkeletonComponent`.
- `ShowSkeleton` debug flag draws the bind-pose skeleton as debug lines, transformed by the entity.
- `SkeletonComponent` round-trips in `world.json` (raw stable-hash handle).
- `test_skeleton` (hierarchy math) green; full tree builds; the existing five suites stay green.
- No `GAME_API_VERSION` bump. `ECS.h` X-macro change ⇒ rebuild ecs+Engine+editor+game + editor
  restart.

## Notes

- `inverseBind` is captured now (unused until SP2 skinning) so SP2 reuses the asset with no
  re-extraction. The `SkeletonStore` is deliberately engine-level + immutable + handle-keyed so SP3's
  GameThread sampling and SP1's render-side viz both read it without sync.
- Reuses the asset-identity machinery: `AssetKeyHash`, `NormalizeAssetKey`, the `#suffix` synth-key
  convention, and (for persistence) the `AssetRegistry` reverse-lookup pattern. See
  [[project_id_stability]].
- SP2 will add `SkinnedMeshVertex` + skinned PSO + skinning shader + the palette transport channel;
  SP3 adds `AnimationClip` + GameThread sampling + game-owned `AnimationComponent`. Those are separate
  specs.
