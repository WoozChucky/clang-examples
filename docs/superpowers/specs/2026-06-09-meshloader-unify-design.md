# MeshLoader Unification Design

**Status:** Design approved, pre-implementation.
**Branch:** `refactor/meshloader-unify`
**Motivation:** Two duplicated assimp loaders diverged and caused a real bug. The static `MeshLoader::LoadMeshFromFile` (editor `MeshManagerPanel`) and the inline async loader in `GameThread.cpp` (runtime model loads) both walk an assimp scene into vertices/indices/submeshes, but diverged on: (1) **index offset** — the skinned/GameThread path builds *local* per-mesh indices with no `baseVertex` offset, so a skinned model with >1 submesh scrambles (latent; the single-submesh Fox is unaffected); (2) **postprocess flags / `aiProcess_FlipUVs`** — the skinned path had the flip commented out, which scrambled the Fox texture (just fixed, `a3f8a55`). Both are symptoms of the duplication.

## Goal

Make **`MeshLoader` the single owner of all assimp scene-walking** — geometry, skeleton, skinning weights, animation clips, and material pixel decode. `GameThread` stops parsing models; it only orchestrates threading, GPU staging, the `.animctrl.json` sidecar, and ECS. Both latent bugs are fixed *by construction*: one global-index walk, one postprocess-flag set, used by every consumer.

## Boundary (who owns what)

- **`MeshLoader` owns:** `Assimp::Importer::ReadFile` + the shared postprocess-flag set, the node/mesh walk producing vertices + **global** indices + submeshes, `AiToGlm` (moved from GameThread), skeleton extraction, skinning-weight extraction, animation-clip extraction, and material **pixel decode** (via the existing `MaterialLoader::LoadMaterialFromFile`). Returns plain CPU data; no GPU, no staging pool, no threading.
- **`GameThread` keeps:** the model-load worker thread + job queue, copying returned material pixels into its staging pool, the sibling `.animctrl.json` JSON load (a file-naming convention, not assimp scene-walking — stays here by explicit decision), assembling `ModelLoadResult` from the `LoadedModel`, and the ECS/completed-job push.
- **Editor `MeshManagerPanel`** calls the same `MeshLoader` entry; uses `vertices`/`indices`/`subMeshes`/`materials`, ignores skeleton/skinning/clips (static import).

## API

Replace `LoadMeshFromFile` (out-params) with one struct-returning entry in `MeshLoader` (`src/engine/src/utilities/MeshLoader.{h,cpp}`):

```cpp
namespace MeshLoader {
  struct MeshMaterial {        // unchanged (already in MeshLoader.h)
    uint32_t Width{0}, Height{0}, MaterialIndex{0};
    std::vector<uint32_t> TextureData; // RGBA8 pixels
  };
  struct LoadedModel {
    std::vector<MeshVertex>    vertices;
    std::vector<uint32_t>      indices;     // GLOBAL: baseVertex + face index, per submesh
    std::vector<SubMesh>       subMeshes;
    std::vector<MeshMaterial>  materials;    // decoded pixel vectors (as today)
    bool                       hasSkeleton = false;
    Skeleton                   skeleton;
    std::vector<SkinnedVertex> skinning;     // aligned 1:1 to vertices; empty if !hasSkeleton
    std::vector<AnimationClip> clips;
  };
  ENGINE_API bool LoadModel(const char* filePath, LoadedModel& out, std::string& outError);
}
```

A single file-scope constant for the postprocess flags:
```cpp
constexpr unsigned kAssimpFlags =
    aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_FlipUVs | aiProcess_JoinIdenticalVertices;
```
Indices are built the static path's way (`baseVertex + face.mIndices[j]`, `baseVertex = outVertices.size()` before appending each mesh) so concatenated submeshes are globally correct — fixing the multi-submesh scramble for every consumer.

`MeshLoader` gains includes for `Skeleton.h`, `AnimationClip.h`, `Skinning.h` (all in `src/common/include`, already reachable from Engine). No new link dependency: `MeshLoader` is already in Engine and Engine already links assimp.

## What moves out of `GameThread.cpp`

Into `MeshLoader::LoadModel`, preserving the **same single walk order** (critical — the skinning-weight `vertexId` base accumulation must see vertices in the same concatenation order as the geometry walk):
- the `processMesh` / `processNode` lambdas (geometry + per-submesh material decode);
- `AiToGlm` (static helper at `GameThread.cpp:53`);
- the skeleton extraction (bone collection + node-tree walk + `rootTransform`);
- the skinning-weight `collect` (→ `MakeSkinnedVertex`, aligned to vertices);
- the animation-clip extraction (channels/keys → `AnimationClip`).

The GameThread worker shrinks to: `MeshLoader::LoadModel(job.objPath, model, err)` → copy `model.materials` pixels into the staging pool (preserving today's single-texture/last-wins mapping into `ModelLoadResult.Texture`/`Width`/`Height`) → load the sibling `.animctrl.json` (unchanged) → fill `ModelLoadResult` (`vertices`/`indices`/`subMeshes`/`skeleton`/`hasSkeleton`/`skeletonKey`/`skinning`/`clips`) from `model` → push to the completed queue.

## Texture seam

`MeshLoader` returns decoded pixels as `vector<MeshMaterial>` — no staging-pool dependency, keeping it free of renderer/threading concerns. `GameThread` copies those into its staging pool exactly as today (same `GetStagingPool().Acquire` + `memcpy`, same last-texture-wins into the single `ModelLoadResult.Texture`). The editor consumes the vectors directly (its existing path). The multi-texture-per-mesh limitation is **preserved as-is** (out of scope).

## Editor consumer

`MeshManagerPanel.cpp` (the only `LoadMeshFromFile` caller) switches to `LoadModel` and reads `model.vertices/indices/subMeshes/materials`. Behavior identical (it already got global indices + flip from the static path). `LoadMeshFromFile` is removed (no other callers).

## Testing

New `tests/test_meshloader.cpp` (links `Engine`, which provides `MeshLoader` + transitively assimp), assert-style `main()` like the other suites:
- **`assets/models/Fox.gltf`** via `LoadModel`: `vertices.size() > 0`; **every index `< vertices.size()`** (regression guard for global-index correctness); `hasSkeleton == true`; `skinning.size() == vertices.size()`; `clips.size() > 0`.
- **`assets/models/cube-textured-multiple.obj`** (multi-submesh static): `subMeshes.size() > 1`; every index `< vertices.size()` (locks the baseVertex offset on the concatenation path); `materials` non-empty.
- **Asset path resolution:** mirror how existing Engine-linked tests locate assets, or pass the source `assets/` dir via a `target_compile_definitions` string (e.g. `MESHLOADER_TEST_ASSETS_DIR`). The plan picks one after checking an existing asset-using test; if no clean path exists, fall back to a build-dir-relative path (assets are copied next to test exes by the CMake post-build step).
- Existing suites stay green; editor + runtime build; manual smoke: Fox still correct, editor manual import still works.

## Scope

**In:** `MeshLoader::LoadModel` + `LoadedModel`; move skeleton/skinning/animation/`AiToGlm` from GameThread into MeshLoader; global indices everywhere; one shared postprocess-flag constant; update GameThread worker + editor panel to the new API; remove `LoadMeshFromFile`; `test_meshloader`.

**Out:** the async/threading model, the staging pool, GPU upload, the multi-texture-per-mesh limitation (preserved), the `.animctrl.json` sidecar convention (stays in GameThread), any change to `MaterialLoader`.

## Risks / notes

- **Walk-order preservation:** skinning weights are indexed by assimp `vertexId` accumulated over the same mesh concatenation order as the geometry. The move keeps one single walk feeding both, so order is preserved by construction — but the implementation must not reorder meshes or split the walk.
- **Includes/linkage:** `MeshLoader` gains `Skeleton.h`/`AnimationClip.h`/`Skinning.h` + more assimp headers; all already available to Engine. No new external link.
- **`ENGINE_API` surface:** `LoadModel` is exported (editor in `editor.exe` calls into `Engine.dll`), matching the current `LoadMeshFromFile` export.
- **Behavior parity for textures:** GameThread's single-texture/last-wins mapping is preserved; only the decode *source* moves (now from `LoadedModel.materials` instead of an inline `MaterialLoader` call). Net pixels identical.
- **`.animctrl.json` stays in GameThread** by explicit decision (it's a sibling-file convention, not scene parsing).
