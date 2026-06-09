# MeshLoader Unification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `MeshLoader` the single owner of all assimp scene-walking (geometry + skeleton + skinning + animation + material pixels); `GameThread` and the editor both call it. Fixes the multi-submesh local-index bug and the postprocess-flag/FlipUVs divergence by construction.

**Architecture:** Add `MeshLoader::LoadModel(path) → LoadedModel` containing everything an importer produces, built with one shared postprocess-flag set and **global** indices. Migrate the `GameThread` async worker and the editor `MeshManagerPanel` to it; delete the inline assimp walk in `GameThread.cpp` and the old `LoadMeshFromFile`. `GameThread` keeps only threading, staging-pool texture copy, the `.animctrl.json` sidecar, and ECS.

**Tech Stack:** C++23, assimp (already linked into `Engine.dll`), `MaterialLoader` (stb_image) for texture decode, GLM, nlohmann::json (sidecar). Build/test preset `msvc-win64-vs2026-community`. Tests are assert-style `main()` exes.

**Spec:** `docs/superpowers/specs/2026-06-09-meshloader-unify-design.md`

**Commit identity (every commit):** `git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit ...`. Never `--no-verify`. Stage exact paths (never `git add -A`/`.`).

**Build/run commands:**
- Build editor (also builds Engine): `cmake --build --preset msvc-win64-vs2026-community --target editor`
- Build a test: `cmake --build --preset msvc-win64-vs2026-community --target test_meshloader`
- Run it: `./out/build/msvc-win64-vs2026-community/bin/Debug/test_meshloader.exe` (expected final line: `All meshloader tests passed.`)

**Reference — current source (read before editing; this is a MOVE, preserve logic exactly):**
- Static loader: `src/engine/src/utilities/MeshLoader.cpp` (whole file) + `MeshLoader.h`. Already does **global** indices (`baseVertex + face.mIndices[j]`, `MeshLoader.cpp:71`), `aiProcess_FlipUVs`, material decode via `MaterialLoader::LoadMaterialFromFile`.
- Inline async loader: `src/engine/src/threading/GameThread.cpp:923-1230` — assimp includes (923-925), `WorkerThreadFunc` (927), `processMesh`/`processNode` lambdas (973-1076, **local** indices at :1003), skeleton extraction (1078-1133), skinning weights (1134-1163), animation clips (1165-1199), sidecar `.animctrl.json` (1204-1221), result push (1223-1228). `AiToGlm` static helper at `GameThread.cpp:53`.
- `ModelLoadResult` struct: `GameThread.h:47-69` (fields: vertices, indices, subMeshes, Width/Height/Texture, skeleton/hasSkeleton/skeletonKey, skinning, clips, hasController/controller/controllerSourcePath, …).
- Editor caller: `MeshManagerPanel.cpp:194` (`LoadMeshFromFile`, the ONLY caller).
- Test CMake pattern: `tests/CMakeLists.txt:375-393` (test_settings links `Engine`).

---

### Task 1: Add `MeshLoader::LoadModel` + `LoadedModel` (additive; TDD via new test)

Add the unified loader to `MeshLoader` WITHOUT removing anything from `GameThread` or the old `LoadMeshFromFile` yet — build stays green (new symbol, no caller change). Write the test first.

**Files:**
- Test: `tests/test_meshloader.cpp` (create), `tests/CMakeLists.txt` (add target)
- Modify: `src/engine/src/utilities/MeshLoader.h` (add `LoadedModel` + `LoadModel`)
- Modify: `src/engine/src/utilities/MeshLoader.cpp` (implement `LoadModel`)

- [ ] **Step 1: Write the failing test**

Create `tests/test_meshloader.cpp`:

```cpp
#include <cstdio>
#include <string>
#include "MeshLoader.h"

static int g_Failures = 0;
#define EXPECT(cond) do { if(!(cond)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); ++g_Failures; } } while(0)

#ifndef MESHLOADER_TEST_ASSETS_DIR
#define MESHLOADER_TEST_ASSETS_DIR "assets"
#endif
static std::string asset(const char* rel) { return std::string(MESHLOADER_TEST_ASSETS_DIR) + "/" + rel; }

// Skinned glTF: geometry + skeleton + skinning + clips all present, indices in range.
static void T01_fox_gltf()
{
    MeshLoader::LoadedModel m; std::string err;
    const bool ok = MeshLoader::LoadModel(asset("models/Fox.gltf").c_str(), m, err);
    EXPECT(ok);
    EXPECT(m.vertices.size() > 0);
    EXPECT(m.indices.size() > 0);
    for (uint32_t idx : m.indices) EXPECT(idx < m.vertices.size()); // global-index correctness
    EXPECT(m.hasSkeleton);
    EXPECT(m.skinning.size() == m.vertices.size());
    EXPECT(m.clips.size() > 0);
}

// Multi-submesh static OBJ: >1 submesh, indices still globally in range (locks baseVertex offset).
static void T02_multi_submesh_obj()
{
    MeshLoader::LoadedModel m; std::string err;
    const bool ok = MeshLoader::LoadModel(asset("models/cube-textured-multiple.obj").c_str(), m, err);
    EXPECT(ok);
    EXPECT(m.subMeshes.size() > 1);
    EXPECT(m.vertices.size() > 0);
    for (uint32_t idx : m.indices) EXPECT(idx < m.vertices.size());
}

int main()
{
    T01_fox_gltf();
    T02_multi_submesh_obj();
    if (g_Failures == 0) { std::printf("All meshloader tests passed.\n"); return 0; }
    std::fprintf(stderr, "%d meshloader test(s) failed.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 2: Add the test target to `tests/CMakeLists.txt`**

Append (mirror the `test_settings` block at lines 375-393; the `MESHLOADER_TEST_ASSETS_DIR` define points the test at the source assets so it doesn't depend on cwd):

```cmake
add_executable(test_meshloader
    test_meshloader.cpp
)

target_link_libraries(test_meshloader PRIVATE
    CommonHeaders
    Engine
)

target_include_directories(test_meshloader PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/engine/src
)

target_compile_definitions(test_meshloader PRIVATE
    MESHLOADER_TEST_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets"
)

set_target_properties(test_meshloader PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 3: Build the test, verify it FAILS to compile**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_meshloader`
Expected: FAILS — `LoadModel` / `LoadedModel` not declared. (Confirms the test exercises the new API.)

- [ ] **Step 4: Declare `LoadedModel` + `LoadModel` in `MeshLoader.h`**

In `src/engine/src/utilities/MeshLoader.h`, add the needed includes and, inside `namespace MeshLoader`, after the `MeshMaterial` struct:

```cpp
#include "Skeleton.h"
#include "AnimationClip.h"
#include "Skinning.h"
```

```cpp
    struct LoadedModel {
        std::vector<MeshVertex>    vertices;
        std::vector<uint32_t>      indices;     // GLOBAL: baseVertex + face index, per submesh
        std::vector<SubMesh>       subMeshes;
        std::vector<MeshMaterial>  materials;
        bool                       hasSkeleton = false;
        Skeleton                   skeleton;
        std::vector<SkinnedVertex> skinning;     // aligned 1:1 to vertices; empty if !hasSkeleton
        std::vector<AnimationClip> clips;
    };

    // Single owner of assimp scene-walking: geometry (global indices) + skeleton + skinning
    // + animation clips + decoded material pixels. No GPU / staging / threading concerns.
    ENGINE_API bool LoadModel(const char* filePath, LoadedModel& out, std::string& outError);
```

- [ ] **Step 5: Implement `LoadModel` in `MeshLoader.cpp`**

In `src/engine/src/utilities/MeshLoader.cpp`, add assimp + glm + the moved logic. This is a **port of the existing GameThread loader** (`GameThread.cpp:927-1199`) plus the static loader's **global-index** rule. Add includes at top:

```cpp
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <functional>
#include <unordered_map>
#include "Skeleton.h"
#include "AnimationClip.h"
#include "Skinning.h"
```

Add a file-scope flag constant + a static `AiToGlm` (copy of `GameThread.cpp:53-60`, kept static — separate TU from GameThread's, no ODR clash):

```cpp
namespace {
constexpr unsigned kAssimpFlags =
    aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_FlipUVs | aiProcess_JoinIdenticalVertices;

glm::mat4 AiToGlm(const aiMatrix4x4& m) {
    return glm::mat4(m.a1,m.b1,m.c1,m.d1, m.a2,m.b2,m.c2,m.d2,
                     m.a3,m.b3,m.c3,m.d3, m.a4,m.b4,m.c4,m.d4);
}
}
```

Implement `LoadModel`:
- `Assimp::Importer importer; const aiScene* scene = importer.ReadFile(filePath, kAssimpFlags);` → on null, set `outError = importer.GetErrorString()`, return false.
- **Geometry walk** — port `processMesh`/`processNode` from `GameThread.cpp:973-1076`, but build **GLOBAL** indices like the static loader (`MeshLoader.cpp:62-75`): for each mesh, `const uint32_t baseVertex = out.vertices.size();` before appending, and push `baseVertex + face.mIndices[j]` (NOT the bare local index — this is the bug fix). Each `SubMesh{ IndexStart = out.indices.size()-before, IndexCount }`. Material decode: for each mesh material with a diffuse texture, resolve a relative path against the model's directory (`std::filesystem::path(filePath).parent_path()`), call `MaterialLoader::LoadMaterialFromFile(...)`, push a `MeshMaterial{Width,Height,MaterialIndex,TextureData}` into `out.materials` (mirror the static loader's material block, `MeshLoader.cpp:78-116`). Keep `subMesh.MaterialIndex` wiring.
- **Skeleton** — port `GameThread.cpp:1078-1133` verbatim (bone inverse-bind collection, node-tree `walk`, `rootTransform`), writing into `out.skeleton` / `out.hasSkeleton`. Use the local `AiToGlm`.
- **Skinning weights** — port `GameThread.cpp:1134-1163` verbatim (`collect` over the SAME walk order, `MakeSkinnedVertex`), writing `out.skinning` (sized `out.vertices.size()`). **Preserve walk order** so `vertexId` bases line up.
- **Animation clips** — port `GameThread.cpp:1165-1199` verbatim, writing `out.clips` (skip channels targeting non-bone nodes via `boneNameToIndex`).
- Return `!out.vertices.empty()`.

> Note: do NOT port the `.animctrl.json` sidecar (1204-1221) or the staging-pool texture copy — those stay in GameThread (Task 2). `LoadModel` only decodes material pixels into `out.materials`.

- [ ] **Step 6: Build + run the test, verify PASS**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_meshloader && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_meshloader.exe`
Expected: `All meshloader tests passed.` (Fox: skeleton+skinning+clips+in-range indices; multi-submesh OBJ: >1 submesh + in-range indices.)

- [ ] **Step 7: Build the editor (ensure additive change didn't break Engine)**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: builds clean (old `LoadMeshFromFile` + new `LoadModel` coexist; GameThread untouched).

- [ ] **Step 8: Commit**

```bash
git add src/engine/src/utilities/MeshLoader.h src/engine/src/utilities/MeshLoader.cpp tests/test_meshloader.cpp tests/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(mesh): MeshLoader::LoadModel single assimp scene-walk (geometry+skeleton+skinning+clips)"
```

---

### Task 2: Migrate the GameThread worker to `LoadModel`; delete the inline loader

Replace the inline assimp walk in `WorkerThreadFunc` with a `MeshLoader::LoadModel` call; keep staging-pool texture copy, the `.animctrl.json` sidecar, and `ModelLoadResult` assembly. Delete the now-dead inline lambdas, skeleton/skinning/anim extraction, GameThread's static `AiToGlm`, and the now-unneeded assimp includes in GameThread.

**Files:**
- Modify: `src/engine/src/threading/GameThread.cpp` (worker body `923-1228`, `AiToGlm` at `53-60`)

- [ ] **Step 1: Replace the worker's load body**

In `src/engine/src/threading/GameThread.cpp`, in `WorkerThreadFunc` after `result.assetKey = job.assetKey;` (`:944`), replace the ENTIRE inline load (the `Assimp::Importer ... ReadFile`, `processMesh`/`processNode`, skeleton/skinning/animation blocks — `:946` through the end of the animation extraction at `:1200`) with:

```cpp
        MeshLoader::LoadedModel model;
        std::string loadErr;
        if (!MeshLoader::LoadModel(job.objPath.c_str(), model, loadErr)) {
            SM_ERROR("MeshLoader failed for '%s': %s", job.objPath.c_str(), loadErr.c_str());
            result.success = false;
            result.error = loadErr;
            { std::scoped_lock lg(m_JobMutex); m_CompletedJobs.push(std::move(result)); }
            continue;
        }

        result.vertices  = std::move(model.vertices);
        result.indices   = std::move(model.indices);
        result.subMeshes = std::move(model.subMeshes);
        result.skeleton    = std::move(model.skeleton);
        result.hasSkeleton = model.hasSkeleton;
        if (result.hasSkeleton) result.skeletonKey = result.assetKey + "#skeleton";
        result.skinning = std::move(model.skinning);
        result.clips    = std::move(model.clips);

        // Texture: copy the decoded pixels into the staging pool (preserve today's single-texture,
        // last-wins behavior into ModelLoadResult.Texture/Width/Height).
        for (const auto& mat : model.materials) {
            if (mat.TextureData.empty()) continue;
            result.Width  = mat.Width;
            result.Height = mat.Height;
            if (result.Texture) { GetStagingPool().Return(result.Texture); result.Texture = nullptr; }
            const size_t texBytes = static_cast<size_t>(mat.Width) * mat.Height * sizeof(uint32_t);
            result.Texture = static_cast<uint32_t*>(GetStagingPool().Acquire(texBytes));
            std::memcpy(result.Texture, mat.TextureData.data(), texBytes);
        }
```

Leave the existing `.animctrl.json` sidecar block (`:1204-1221`), `result.success = result.vertices.size() > 0;` (`:1223`), and the completed-job push (`:1225-1228`) intact — they follow this code unchanged.

- [ ] **Step 2: Delete the now-dead `AiToGlm` + assimp includes in GameThread**

- Remove the static `AiToGlm` at `GameThread.cpp:53-60` (now in MeshLoader; GameThread no longer walks the scene).
- Remove the assimp includes that were only for the inline loader: `#include <assimp/Importer.hpp>` and `#include <assimp/postprocess.h>` at `:923-924` (keep `<filesystem>` at `:925` — the sidecar uses it). If any assimp header is referenced elsewhere in the file, keep it; grep `GameThread.cpp` for `assimp`/`ai` usage after removal to confirm none remain except none.
- Add `#include "MeshLoader.h"` to GameThread.cpp's includes if not already present.

- [ ] **Step 3: Build the editor**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: builds clean. (GameThread no longer references assimp or `AiToGlm`; `Skeleton`/`AnimationClip`/`SkinnedVertex` still come via `MeshLoader.h`/existing includes.)

- [ ] **Step 4: Re-run the meshloader + a broad test sanity**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_meshloader && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_meshloader.exe`
Expected: `All meshloader tests passed.`

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/threading/GameThread.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "refactor(mesh): GameThread worker uses MeshLoader::LoadModel; drop inline assimp walk"
```

---

### Task 3: Migrate the editor panel; remove `LoadMeshFromFile`

Switch the one editor caller to `LoadModel` and delete the now-unused `LoadMeshFromFile` (declaration + definition).

**Files:**
- Modify: `src/editor/src/panels/MeshManagerPanel.cpp` (`:188-212` region)
- Modify: `src/engine/src/utilities/MeshLoader.h` (remove `LoadMeshFromFile` decl)
- Modify: `src/engine/src/utilities/MeshLoader.cpp` (remove `LoadMeshFromFile` def + its now-unused static `ProcessMesh`/`ProcessNode` helpers if they're not used by `LoadModel`)

- [ ] **Step 1: Switch the editor caller**

In `src/editor/src/panels/MeshManagerPanel.cpp`, replace the load block (`:188-194`):

```cpp
                std::vector<MeshVertex> vertices;
                std::vector<uint32_t> indices;
                std::vector<MeshLoader::MeshMaterial> materials;
                std::vector<SubMesh> subMeshes;
                std::string error;

                if (MeshLoader::LoadMeshFromFile(filePath, vertices, indices, subMeshes, materials, error)) {
```

with:

```cpp
                MeshLoader::LoadedModel model;
                std::string error;

                if (MeshLoader::LoadModel(filePath, model, error)) {
                    auto& vertices  = model.vertices;
                    auto& indices   = model.indices;
                    auto& subMeshes = model.subMeshes;
                    auto& materials = model.materials;
```

(The rest of the block — `AddMesh(... vertices.data() ... indices.data() ... subMeshes ...)`, the `materials` loop, the status message using `vertices.size()`/`indices.size()` — works unchanged against these references. Verify the closing brace balance after adding the inner aliases; the aliases are scoped inside the `if`.)

- [ ] **Step 2: Remove `LoadMeshFromFile`**

- In `src/engine/src/utilities/MeshLoader.h`, delete the `LoadMeshFromFile` declaration (`:22-29`).
- In `src/engine/src/utilities/MeshLoader.cpp`, delete the `LoadMeshFromFile` definition (`:142-194`). If the file-scope `ProcessMesh`/`ProcessNode` helpers (`:13-140`) are no longer referenced (Task 1's `LoadModel` brought its own walk), delete them too; if `LoadModel` was implemented to reuse them, keep them. Grep the file to confirm no dangling references before deleting.

- [ ] **Step 3: Build the editor**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: builds clean. No remaining `LoadMeshFromFile` references (grep `src/` to confirm zero).

- [ ] **Step 4: Build runtime (no overlay) to confirm Engine consumers compile**

Run: `cmake --build --preset msvc-win64-vs2026-community --target runtime`
Expected: builds clean.

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/panels/MeshManagerPanel.cpp src/engine/src/utilities/MeshLoader.h src/engine/src/utilities/MeshLoader.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "refactor(mesh): editor uses MeshLoader::LoadModel; remove LoadMeshFromFile"
```

---

## Final review (after all tasks)

Dispatch a whole-branch spec + quality review per subagent-driven-development, then `superpowers:finishing-a-development-branch` for the FF-merge to `main`. Confirm before pushing.

**Whole-branch smoke (user-run):**
- Fox still textures correctly + animates (the just-fixed flip preserved through the move).
- Editor → Mesh Manager → manual import of an OBJ (e.g. `cube-textured.obj`) and a glTF still loads + textures.
- A skinned model still skins/animates in Play.
- `test_meshloader` green; existing suites green.

## Notes / gotchas

- **Walk-order preservation (critical):** skinning weights index by assimp `vertexId` accumulated over the geometry walk's mesh order. `LoadModel` must keep ONE walk feeding geometry then weights in the same order (port preserves this). Reordering or splitting the walk silently scrambles skinning.
- **Index fix is the behavioral change:** GameThread's old path used local indices; `LoadModel` uses global. Single-submesh models (Fox) are byte-identical; multi-submesh skinned models are now correct (the latent fix). The `T02` multi-submesh test guards it.
- **Two static `AiToGlm`s briefly coexist** (MeshLoader's new one + GameThread's old one) between Task 1 and Task 2 — different TUs, both `static`/anonymous-namespace, no ODR clash. Task 2 deletes GameThread's.
- **Texture parity:** only the decode *source* moves (now `LoadedModel.materials` vs inline `MaterialLoader`); the staging-pool copy + single-texture/last-wins mapping stay in GameThread, byte-identical pixels.
- **No new linkage:** `MeshLoader` is already in `Engine` and Engine already links assimp; `Skeleton.h`/`AnimationClip.h`/`Skinning.h` are common headers.
- **`ENGINE_API`:** `LoadModel` is exported (the editor in `editor.exe` calls into `Engine.dll`), matching the old `LoadMeshFromFile` export.
