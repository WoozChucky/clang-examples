# Animation SP1 — Skeleton Import + Visualization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load a rigged glTF, extract its skeleton (bones + hierarchy + inverse-bind) into a keyed immutable CPU asset, and draw the bind-pose skeleton as debug lines for entities with a `SkeletonComponent`, gated by a `ShowSkeleton` flag.

**Architecture:** Six tasks: (1) `Skeleton` asset + pure bind-pose math + unit test; (2) `SkeletonStore` singleton (engine, GameThread-written / render-read, mutex-guarded, mirrors `NavMeshSystem::Instance()`); (3) `SkeletonComponent` engine builtin + round-trip test; (4) assimp extraction in `WorkerThreadFunc` + GameThread registration + the test asset; (5) `ShowSkeleton` debug-flag viz in `DebugRenderPass`; (6) regression + manual smoke. No skinning/shaders/clips (SP2/SP3).

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-community`), CMake, assimp, glm, nlohmann/json, NVRHI. Reuses `AssetKeyHash`/`NormalizeAssetKey` + the `#suffix` synth-key convention from the asset-identity work.

**Scope:** Implements `docs/superpowers/specs/2026-06-07-anim-skeleton-import-design.md` (sub-project 1 of the animation effort). `ECS.h` X-macro change ⇒ rebuild **ecs + Engine + editor + game** + editor restart. No `GAME_API_VERSION` bump.

> **Branch:** Work happens on `feat/anim-skeleton-import` (already created off `main`). Stay on it.

---

## Background facts (verified — current code)

- **Line numbers approximate — locate edits by quoted code.**
- `WorkerThreadFunc` (`GameThread.cpp:727-852`): assimp `ReadFile(job.objPath, aiProcess_Triangulate|GenSmoothNormals|JoinIdenticalVertices)`. `processMesh` lambda fills `MeshVertex` (pos/normal/uv) + indices; `processNode` recurses the `aiNode` tree appending sub-meshes; then `processNode(scene->mRootNode, scene, result.vertices, result.indices); result.success = ...; m_CompletedJobs.push(std::move(result));`. **No `aiMatrix4x4`→glm conversion exists** (node transforms not applied) — SP1 introduces one. `job.assetKey` (set in the asset-identity work) holds the normalized model key; the worker already copies it to `result.assetKey`.
- `ModelLoadResult` (`GameThread.h:42-56`): has `ticketId, assetKey, success, error, vertices, indices, subMeshes, Width/Height/Texture, MeshUploaded`.
- Completed-jobs drain (`GameThread.cpp:346-428`): `ModelLoadResult res = std::move(local.front());` then `if (!res.success) continue;`, builds `RequestMesh`/`RequestMaterial` commands, can `requeueAndStop(res, local)` on a full ring.
- `DebugRenderPass::Render(cmdList, fb, snapshot, const ECS* world, dt, frameAlloc)` (`DebugRenderPass.cpp:90+`): reads global `GetDebugDrawSettings()` (NOT the snapshot), early-outs if all flags off, fills member `std::vector<DebugVertex> m_Verts`. Reaches systems via `m_Renderer` (`GetMeshSystem()`, `GetAppContext()`) and singletons (`NavMeshSystem::Instance()`, used in the ShowNavMesh block `:198`). Selected-AABB block (`:137`) shows the `world->GetComponent<>` + `DebugAppendBox` pattern.
- `DebugDrawSettings` (`RenderStats.h:17-28`): bool flags (`ShowGrid` etc.) + `ENGINE_API DebugDrawSettings& GetDebugDrawSettings();` (`:57`). Set via checkboxes in `RenderStatsPanel.cpp:26-42`; `ImGuiRenderer.cpp:185` defaults `ShowGrid=true`.
- `DebugVertex { glm::vec3 Position; glm::vec4 Color; }` + `DebugAppendLine(out, a, b, color)` (`DebugDraw.h:6-16`).
- `SerializerRegistry()` builtin block ends with `r.Register<NameComponent>("NameComponent", true);` (`ComponentSerializers.cpp:6-29`).
- X-macro tail (`ECS.h:395-402`) ends `X(NameComponent)` (no trailing backslash).
- `NavMeshSystem` (`NavMeshSystem.h:21-25,90`): `static NavMeshSystem& Instance();` + private default ctor (singleton). GameThread-written, render-read — the precedent `SkeletonStore` mirrors.
- `Renderer` builds passes in `Renderer::Init` (`Renderer.cpp:138-143`) AND `InitForSwap` (`:773-775`): `auto p = std::make_unique<DebugRenderPass>(); p->Initialize(m_Device, this); AddRenderPass(...)`.
- `world->Each<A,B>([](EntityId, A&, B&){...})` is the iteration API used by other passes (e.g. GBuffer `Each<TransformComponent, MeshComponent>`).
- `ModelMatrix(const TransformComponent&)` helper exists (used in DebugRenderPass selected-AABB block) → entity world matrix.

## Type/symbol contract (keep exact)

- `src/common/include/Skeleton.h`: `struct Bone { std::string name; int parent=-1; glm::mat4 localBind{1.0f}; glm::mat4 inverseBind{1.0f}; };`, `struct Skeleton { std::vector<Bone> bones; };`, `std::vector<glm::mat4> ComputeBindPoseGlobals(const Skeleton&)`.
- `src/engine/src/animation/SkeletonStore.{h,cpp}`: singleton `static SkeletonStore& Instance();`, `uint64_t Add(const std::string& key, Skeleton sk)`, `const Skeleton* Get(uint64_t handle) const`, `std::string KeyForHandle(uint64_t) const`.
- `struct SkeletonComponent { uint64_t SkeletonId = 0; };` in `ECS.h`; key `"SkeletonComponent"`.
- `ModelLoadResult` gains `Skeleton skeleton; bool hasSkeleton=false; std::string skeletonKey;`.
- `DebugDrawSettings` gains `bool ShowSkeleton = false;`.

---

### Task 1: `Skeleton` asset + bind-pose math + unit test (TDD)

**Files:** Create `src/common/include/Skeleton.h`; Test `tests/test_skeleton.cpp` + `tests/CMakeLists.txt`.

- [ ] **Step 1: Write the failing test (`tests/test_skeleton.cpp`)**
```cpp
#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "Skeleton.h"

static int g_Failures = 0;
#define EXPECT(cond) do { if(!(cond)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); ++g_Failures; } } while(0)
static bool nearf(float a, float b) { return std::fabs(a - b) < 1e-5f; }

int main() {
    Skeleton sk;
    Bone root;  root.name = "root";  root.parent = -1; root.localBind = glm::translate(glm::mat4(1.0f), glm::vec3(1,0,0));
    Bone child; child.name = "child"; child.parent = 0;  child.localBind = glm::translate(glm::mat4(1.0f), glm::vec3(0,2,0));
    sk.bones = { root, child };

    const auto g = ComputeBindPoseGlobals(sk);
    EXPECT(g.size() == 2);
    const glm::vec3 rp = glm::vec3(g[0] * glm::vec4(0,0,0,1));
    const glm::vec3 cp = glm::vec3(g[1] * glm::vec4(0,0,0,1));
    EXPECT(nearf(rp.x,1) && nearf(rp.y,0) && nearf(rp.z,0));     // root at (1,0,0)
    EXPECT(nearf(cp.x,1) && nearf(cp.y,2) && nearf(cp.z,0));     // child = root*local = (1,2,0)
    EXPECT(sk.bones[1].parent < 1);                              // topological invariant: parent index < child

    if (g_Failures == 0) std::printf("All skeleton tests passed.\n");
    return g_Failures ? 1 : 0;
}
```
Register in `tests/CMakeLists.txt` mirroring `test_assetkey` (read it; copy its `add_executable`/`target_link_libraries`/`target_include_directories`/properties shape, substituting `test_skeleton`):
```cmake
add_executable(test_skeleton test_skeleton.cpp)
target_link_libraries(test_skeleton PRIVATE CommonHeaders glm::glm ecs)
target_include_directories(test_skeleton PRIVATE ${CMAKE_SOURCE_DIR}/src/common/include)
```
(Match the exact properties — `RUNTIME_OUTPUT_DIRECTORY`, `FOLDER Tests`, GLM defines — used by `test_assetkey`/`test_reloadpreserve` in that file.)

- [ ] **Step 2: Configure + build — confirm it FAILS**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_skeleton
```
Expected: COMPILE ERROR — `Skeleton.h` not found / `ComputeBindPoseGlobals` undeclared. (TDD red.)

- [ ] **Step 3: Create `src/common/include/Skeleton.h`**
```cpp
#pragma once
#include <vector>
#include <string>
#include <glm/glm.hpp>

// Immutable skeletal-animation asset (CPU data). Bones are topologically ordered so a bone's parent
// index is always < its own index (parent computed before child in ComputeBindPoseGlobals).
// inverseBind (assimp mOffsetMatrix) is unused for SP1 bind-pose drawing — captured now for SP2
// skinning so the asset needs no re-extraction. See docs/superpowers/specs/2026-06-07-anim-skeleton-import-design.md.
struct Bone {
    std::string name;
    int         parent = -1;       // index into Skeleton::bones; -1 = root
    glm::mat4   localBind{1.0f};   // local bind transform (rest pose), relative to parent
    glm::mat4   inverseBind{1.0f}; // mesh-space -> bone-space at bind (SP2)
};

struct Skeleton {
    std::vector<Bone> bones;
};

// Bind-pose global (model-space) transform per bone: global[b] = parent<0 ? localBind : global[parent]*localBind.
// Requires topological order (parent index < b), which the importer guarantees.
inline std::vector<glm::mat4> ComputeBindPoseGlobals(const Skeleton& sk) {
    std::vector<glm::mat4> g(sk.bones.size());
    for (size_t b = 0; b < sk.bones.size(); ++b) {
        const Bone& bone = sk.bones[b];
        g[b] = (bone.parent < 0) ? bone.localBind : g[bone.parent] * bone.localBind;
    }
    return g;
}
```

- [ ] **Step 4: Build + run — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community --target test_skeleton
./out/build/msvc-win64-vs2026-community/bin/Debug/test_skeleton.exe
```
Expected: `All skeleton tests passed.`

- [ ] **Step 5: Commit**
```
git -C C:/dev/clang-examples add src/common/include/Skeleton.h tests/test_skeleton.cpp tests/CMakeLists.txt
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): Skeleton asset + bind-pose hierarchy math + unit test"
```

---

### Task 2: `SkeletonStore` singleton (engine)

**Files:** Create `src/engine/src/animation/SkeletonStore.{h,cpp}`; Modify `src/engine/CMakeLists.txt`.

Build-verified (singleton over Engine.dll; behavior covered by the math test + Task 6 smoke).

- [ ] **Step 1: Create `src/engine/src/animation/SkeletonStore.h`**
```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include "Engine.h"      // ENGINE_API
#include "Skeleton.h"

// Process-wide store of immutable Skeleton assets, keyed by a stable hash handle
// (AssetKeyHash of the logical key, e.g. "models/x.gltf#skeleton"). GameThread-written (Add, at
// model load), render-read (Get, in DebugRenderPass). Entries are immutable once added, but the
// container mutates across loads while render reads — so a mutex guards both Add and Get. Mirrors
// the NavMeshSystem singleton pattern. Asset counts are tiny, so the lock is negligible.
class ENGINE_API SkeletonStore {
public:
    static SkeletonStore& Instance();

    // De-dups by key (re-adding a key returns the existing handle). Returns the stable handle.
    uint64_t Add(const std::string& key, Skeleton skeleton);
    // Pointer valid for the process lifetime (entries are never erased/mutated). Null if unknown.
    const Skeleton* Get(uint64_t handle) const;
    std::string KeyForHandle(uint64_t handle) const;

private:
    SkeletonStore() = default;
    struct Entry { std::string key; Skeleton skeleton; };
    mutable std::mutex m_Mutex;
    std::unordered_map<uint64_t, Entry> m_ByHandle;
};
```

- [ ] **Step 2: Create `src/engine/src/animation/SkeletonStore.cpp`**
```cpp
#include "animation/SkeletonStore.h"
#include "AssetKey.h"
#include "lib.h"        // SM_WARN

SkeletonStore& SkeletonStore::Instance() {
    static SkeletonStore s;
    return s;
}

uint64_t SkeletonStore::Add(const std::string& key, Skeleton skeleton) {
    const uint64_t handle = AssetKeyHash(key);
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    if (it != m_ByHandle.end()) return handle; // de-dup
    m_ByHandle.emplace(handle, Entry{ key, std::move(skeleton) });
    return handle;
}

const Skeleton* SkeletonStore::Get(uint64_t handle) const {
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    return it == m_ByHandle.end() ? nullptr : &it->second.skeleton;
}

std::string SkeletonStore::KeyForHandle(uint64_t handle) const {
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    return it == m_ByHandle.end() ? std::string() : it->second.key;
}
```
NOTE: `Get` returns a pointer into the map while only locking during lookup — safe ONLY because entries are never erased or mutated after insert (the pointed-to `Skeleton` is stable for process lifetime; the lock just protects the map's structure during the find against a concurrent `Add` rehash). Do not add erase/replace without revisiting this.
Confirm the include path: `SkeletonStore.cpp` includes `"animation/SkeletonStore.h"` and `"AssetKey.h"`/`"Skeleton.h"` (common). Check how engine sources include common headers (the engine target already adds `src/common/include` to its include path — verify; `AssetKey.h` is used by MeshSystem.cpp the same way). If `"animation/SkeletonStore.h"` doesn't resolve, use the include style the neighboring engine `.cpp`s use for their own headers.

- [ ] **Step 3: Add to `src/engine/CMakeLists.txt`**
Find the engine target's explicit source list (the `.cpp`s under `src/engine/src/...`). Add `src/engine/src/animation/SkeletonStore.cpp` alongside them (match the existing relative-path style in that list).

- [ ] **Step 4: Build `Engine` — GREEN**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target Engine
```
Expected: builds clean (`Engine.dll` links).

- [ ] **Step 5: Commit**
```
git -C C:/dev/clang-examples add src/engine/src/animation/SkeletonStore.h src/engine/src/animation/SkeletonStore.cpp src/engine/CMakeLists.txt
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): SkeletonStore singleton (immutable, handle-keyed, mutex-guarded)"
```

---

### Task 3: `SkeletonComponent` engine builtin + round-trip test (TDD)

**Files:** `src/common/include/ECS.h`, `src/common/include/ComponentSerialization.h`, `src/ecs/src/ComponentSerializers.cpp`, `src/common/include/ECSCommands.h`, `tests/test_worldserial.cpp`.

- [ ] **Step 1: Write the failing test (`tests/test_worldserial.cpp`)**
Add after `T12_asset_key_roundtrip()` (find its closing brace):
```cpp
static void T13_skeleton_roundtrip()
{
    SkeletonComponent in; in.SkeletonId = 0x00ABCDEF12345678ull;
    const nlohmann::json j = in;
    EXPECT(j.at("SkeletonId").get<uint64_t>() == in.SkeletonId);
    const auto out = j.get<SkeletonComponent>();
    EXPECT(out.SkeletonId == in.SkeletonId);

    SkeletonComponent def;
    EXPECT(def.SkeletonId == 0ull);
}
```
Register `    T13_skeleton_roundtrip();` in `main()` after the `T12_asset_key_roundtrip();` line.

- [ ] **Step 2: Build the test — confirm it FAILS**
```
cmake --build --preset msvc-win64-vs2026-community --target test_worldserial
```
Expected: COMPILE ERROR — `SkeletonComponent` undeclared. (TDD red.)

- [ ] **Step 3: Declare the struct + X-macro entry (`src/common/include/ECS.h`)**
Add the struct near the other component declarations (e.g. right after the `NameComponent` struct — locate `struct NameComponent`):
```cpp

// Binds an entity to a Skeleton asset (in SkeletonStore) by stable hash handle (0 = none). Engine
// builtin: the model-load path attaches it for rigged glTFs; DebugRenderPass draws the bind pose.
// The handle is a stable AssetKeyHash, so persisting it raw round-trips across runs.
struct SkeletonComponent {
    uint64_t SkeletonId = 0;
};
```
Then extend the X-macro tail:
```cpp
    X(NameComponent) \
    X(SkeletonComponent)
```
(Add ` \` after `X(NameComponent)`; `X(SkeletonComponent)` is the new last line, no trailing backslash.)

- [ ] **Step 4: Serializer (`src/common/include/ComponentSerialization.h`)**
Add (next to the other component serializers):
```cpp
inline void to_json(nlohmann::json& j, const SkeletonComponent& t) {
    j = nlohmann::json{ {"SkeletonId", t.SkeletonId} };
}
inline void from_json(const nlohmann::json& j, SkeletonComponent& t) {
    j.at("SkeletonId").get_to(t.SkeletonId);
}
```

- [ ] **Step 5: Register builtin (`src/ecs/src/ComponentSerializers.cpp`)**
After `r.Register<NameComponent>("NameComponent", true);` add:
```cpp
        r.Register<SkeletonComponent>("SkeletonComponent", true);
```

- [ ] **Step 6: Typed command branches (`src/common/include/ECSCommands.h`)**
In `ApplyComponentCommand`, find an existing builtin branch (e.g. `ColliderComponent`) and add alongside it:
```cpp
        } else if (componentData.Type == std::type_index(typeid(SkeletonComponent))) {
            if (auto* s = componentData.Get<SkeletonComponent>()) {
                world.AddComponent(entity, *s);
            }
```
In `RemoveComponentByType`, add alongside the matching chain:
```cpp
        } else if (typeIndex == std::type_index(typeid(SkeletonComponent))) {
            world.RemoveComponent<SkeletonComponent>(entity);
```
(Match the exact `} else if` brace/indent style of the surrounding branches; keep the chain well-formed.)

- [ ] **Step 7: Build + run — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community --target ecs --target test_worldserial
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
```
Expected: `All world-serialization tests passed.`

- [ ] **Step 8: Commit**
```
git -C C:/dev/clang-examples add src/common/include/ECS.h src/common/include/ComponentSerialization.h src/ecs/src/ComponentSerializers.cpp src/common/include/ECSCommands.h tests/test_worldserial.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(ecs): SkeletonComponent builtin (handle binding) + round-trip test"
```

---

### Task 4: assimp skeleton extraction + GameThread registration + test asset

**Files:** `src/engine/src/threading/GameThread.h`, `src/engine/src/threading/GameThread.cpp`, `assets/models/RiggedSimple.gltf` (+ `.bin`).

- [ ] **Step 1: Fetch the rigged test asset**
Download Khronos `RiggedSimple` (glTF + its binary buffer) into `assets/models/`:
```
curl -L -o C:/dev/clang-examples/assets/models/RiggedSimple.gltf https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/RiggedSimple/glTF/RiggedSimple.gltf
curl -L -o C:/dev/clang-examples/assets/models/RiggedSimple0.bin https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/RiggedSimple/glTF/RiggedSimple0.bin
```
Verify both downloaded (non-HTML, non-empty): `ls -l` and check the `.gltf` is JSON (starts with `{`). Confirm they are NOT gitignored: `git -C C:/dev/clang-examples status --short assets/models/` should list both as untracked (existing `assets/models/*.obj` are tracked, so the dir isn't broadly ignored — but verify). If `.gltf` references files other than `RiggedSimple0.bin` (open it, check `"uri"` fields under `buffers`/`images`), fetch those too from the same glTF dir.

- [ ] **Step 2: Add skeleton fields to `ModelLoadResult` (`GameThread.h`)**
Add `#include "Skeleton.h"` to `GameThread.h`. In `struct ModelLoadResult`, after the existing members add:
```cpp
        Skeleton    skeleton{};
        bool        hasSkeleton{false};
        std::string skeletonKey;
```

- [ ] **Step 3: Add the assimp→glm matrix helper + extraction (`GameThread.cpp`)**
Add includes at the top: `#include "Skeleton.h"`, `#include "AssetKey.h"`, `#include <unordered_map>` (if not already). Add a file-local helper near the top of the anonymous/translation unit:
```cpp
static glm::mat4 AiToGlm(const aiMatrix4x4& m) {
    // assimp is row-major; glm is column-major. Each (a,b,c,d) row below becomes a glm column.
    return glm::mat4(
        m.a1, m.b1, m.c1, m.d1,
        m.a2, m.b2, m.c2, m.d2,
        m.a3, m.b3, m.c3, m.d3,
        m.a4, m.b4, m.c4, m.d4);
}
```
In `WorkerThreadFunc`, AFTER `processNode(scene->mRootNode, scene, result.vertices, result.indices);` and BEFORE `result.success = ...`, insert skeleton extraction:
```cpp
        // --- Skeleton extraction (animation SP1) ---
        {
            // 1) Collect every bone (by name) referenced across the scene's meshes -> inverse-bind.
            std::unordered_map<std::string, glm::mat4> boneInverseBind;
            for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
                const aiMesh* mesh = scene->mMeshes[mi];
                for (unsigned bi = 0; bi < mesh->mNumBones; ++bi) {
                    const aiBone* bone = mesh->mBones[bi];
                    boneInverseBind[bone->mName.C_Str()] = AiToGlm(bone->mOffsetMatrix);
                }
            }
            // 2) Walk the node tree (pre-order => parent emitted before child = topological order).
            //    A node whose name matches a bone becomes a Skeleton bone; parent = nearest ancestor bone.
            if (!boneInverseBind.empty()) {
                Skeleton skel;
                std::function<void(const aiNode*, int)> walk = [&](const aiNode* node, int parentBoneIdx) {
                    int myIdx = parentBoneIdx;
                    auto it = boneInverseBind.find(node->mName.C_Str());
                    if (it != boneInverseBind.end()) {
                        Bone bone;
                        bone.name        = node->mName.C_Str();
                        bone.parent      = parentBoneIdx;
                        bone.localBind   = AiToGlm(node->mTransformation);
                        bone.inverseBind = it->second;
                        myIdx = static_cast<int>(skel.bones.size());
                        skel.bones.push_back(std::move(bone));
                    }
                    for (unsigned c = 0; c < node->mNumChildren; ++c)
                        walk(node->mChildren[c], myIdx);
                };
                walk(scene->mRootNode, -1);
                if (!skel.bones.empty()) {
                    result.skeleton    = std::move(skel);
                    result.hasSkeleton = true;
                    result.skeletonKey = result.assetKey + "#skeleton";
                    SM_TRACE("Skeleton extracted: '%s' (%zu bones)", result.skeletonKey.c_str(), result.skeleton.bones.size());
                }
            }
        }
```
(`<functional>` is already included — `processNode` uses `std::function`. `result.assetKey` is the normalized model key set earlier by the worker.)

- [ ] **Step 4: Register the skeleton + attach the component (`GameThread.cpp` completed-jobs drain)**
Add `#include "animation/SkeletonStore.h"` to `GameThread.cpp`. In the completed-jobs drain, immediately after `if (!res.success) { ...; continue; }` (before the mesh-upload command build, so it runs once and isn't tied to the mesh-upload retry), insert:
```cpp
                    // Register the skeleton (idempotent by key) + bind it to the entity (if any).
                    if (res.hasSkeleton) {
                        const uint64_t skelHandle = SkeletonStore::Instance().Add(res.skeletonKey, res.skeleton);
                        if (gameState.World.IsValidEntity(res.ticketId)) {
                            if (!gameState.World.HasComponent<SkeletonComponent>(res.ticketId))
                                gameState.World.AddComponent(res.ticketId, SkeletonComponent{ skelHandle });
                            else
                                gameState.World.Modify<SkeletonComponent>(res.ticketId, [&](auto& s){ s.SkeletonId = skelHandle; });
                        }
                    }
```
(`SkeletonStore::Add` de-dups, so a requeued `res` re-registering is harmless. Startup non-entity loads still register the skeleton in the store — `IsValidEntity` is false, so no component attaches, but the asset exists by handle.)

- [ ] **Step 5: Build full tree — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: clean. (Runtime extraction verified by manual smoke in Task 6.)

- [ ] **Step 6: Commit**
`git -C C:/dev/clang-examples status` — confirm `assets/models/RiggedSimple.gltf` + `RiggedSimple0.bin` are present and intended; NO `world.json`/settings json staged.
```
git -C C:/dev/clang-examples add src/engine/src/threading/GameThread.h src/engine/src/threading/GameThread.cpp assets/models/RiggedSimple.gltf assets/models/RiggedSimple0.bin
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): extract glTF skeleton in WorkerThreadFunc + register in SkeletonStore + attach SkeletonComponent; add RiggedSimple test asset"
```
(If the `.gltf` referenced extra files, add those exact paths too.)

---

### Task 5: `ShowSkeleton` bind-pose visualization

**Files:** `src/engine/src/rendering/RenderStats.h`, `src/editor/src/panels/RenderStatsPanel.cpp`, `src/engine/src/rendering/passes/DebugRenderPass.cpp`.

Build-verified + manual smoke.

- [ ] **Step 1: Add the flag (`RenderStats.h`)**
In `struct DebugDrawSettings`, after `bool ShowNavPaths = false;` add:
```cpp
    bool ShowSkeleton      = false;
```

- [ ] **Step 2: Editor checkbox (`RenderStatsPanel.cpp`)**
After the `ImGui::Checkbox("Nav Paths", &dd.ShowNavPaths);` line add:
```cpp
    changed |= ImGui::Checkbox("Skeleton",       &dd.ShowSkeleton);
```

- [ ] **Step 3: Viz block (`DebugRenderPass.cpp`)**
Add includes at the top: `#include "Skeleton.h"` and `#include "animation/SkeletonStore.h"`.
Extend the early-out guard (`:101`) to include `&& !s.ShowSkeleton`:
```cpp
    if (!s.ShowLightGizmos && !s.ShowCameraFrustum && !s.ShowSelectedAABB && !s.ShowGrid && !s.ShowColliders && !s.ShowNavMesh && !s.ShowObstacles && !s.ShowNavPaths && !s.ShowSkeleton)
        return;
```
Add a viz block (place it near the other `if (s.ShowX)` blocks, e.g. after the ShowNavMesh block):
```cpp
    if (s.ShowSkeleton) {
        const glm::vec4 col(0.2f, 0.9f, 1.0f, 1.0f); // cyan
        world->Each<TransformComponent, SkeletonComponent>(
            [&](EntityId /*e*/, const TransformComponent& tc, const SkeletonComponent& skc) {
                const Skeleton* sk = SkeletonStore::Instance().Get(skc.SkeletonId);
                if (!sk || sk->bones.empty()) return;
                const glm::mat4 world_ = ModelMatrix(tc);
                const std::vector<glm::mat4> globals = ComputeBindPoseGlobals(*sk);
                std::vector<glm::vec3> joints(globals.size());
                for (size_t b = 0; b < globals.size(); ++b)
                    joints[b] = glm::vec3(world_ * globals[b] * glm::vec4(0,0,0,1));
                for (size_t b = 0; b < sk->bones.size(); ++b) {
                    const int p = sk->bones[b].parent;
                    if (p >= 0) DebugAppendLine(m_Verts, joints[p], joints[b], col);
                }
            });
    }
```
(`world->Each<A,B>` signature: confirm whether the callback receives `(EntityId, A&, B&)` or `(A&, B&)` by checking an existing `Each` call in this file or `GBufferFillPass.cpp`, and match it. `ModelMatrix(tc)` is already used in this file. Components are read const from the snapshot.)

- [ ] **Step 4: Build — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community --target Engine --target editor
```
Expected: clean.

- [ ] **Step 5: Commit**
```
git -C C:/dev/clang-examples add src/engine/src/rendering/RenderStats.h src/editor/src/panels/RenderStatsPanel.cpp src/engine/src/rendering/passes/DebugRenderPass.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): ShowSkeleton debug flag draws bind-pose skeleton as debug lines"
```

---

### Task 6: Full regression + manual smoke

**Files:** none (verification; fixups only if needed).

- [ ] **Step 1: Reconfigure + full clean build**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community
```
Expected: all targets build, no errors / `LNK`.

- [ ] **Step 2: Run the suites**
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_skeleton.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_assetkey.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_compserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_reloadpreserve.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_playermove.exe
```
Expected: each prints its pass line (`test_compserial` emits the expected `NopeNotReal` warn; `test_reloadpreserve` the expected `BadComp` warn).

- [ ] **Step 3: Manual smoke (human-owned; editor restart required — `ECS.h` changed)**
With `RiggedSimple.gltf` in `assets/models/`:
1. Start the editor; the model loads. Check the console for `Skeleton extracted: 'models/RiggedSimple.gltf#skeleton' (N bones)`.
2. Select the RiggedSimple entity; in the RenderStats panel toggle **Skeleton** on → bind-pose bones draw as cyan lines at the model's location.
3. Move/rotate the entity (gizmo) → the drawn skeleton follows the entity transform.
4. Confirm a non-rigged model (an existing `.obj`) shows no skeleton + no crash, and toggling Skeleton off costs nothing.
5. Save → reload `world.json`: the rigged entity's `SkeletonComponent` persists (skeleton still draws after reload). An old world without `SkeletonComponent` still loads.

- [ ] **Step 4: Commit fixups (only if needed).**

---

## Done criteria

- `Skeleton.h` + `ComputeBindPoseGlobals` + `test_skeleton` green (Task 1).
- `SkeletonStore` singleton (immutable, handle-keyed, mutex-guarded) builds (Task 2).
- `SkeletonComponent` builtin + serializer + ECSCommands branches; `test_worldserial` T13 green (Task 3).
- glTF skeleton extracted in `WorkerThreadFunc` (bones/parents/localBind/inverseBind via `AiToGlm`), registered in `SkeletonStore`, `SkeletonComponent` attached; `RiggedSimple` asset committed (Task 4).
- `ShowSkeleton` flag draws the bind-pose skeleton transformed by the entity (Task 5).
- Full tree builds; all suites green; manual smoke shows the skeleton (Task 6).
- No `GAME_API_VERSION` bump; `ECS.h` X-macro change ⇒ rebuild ecs+Engine+editor+game + editor restart.

## Notes

- `inverseBind` is captured but unused in SP1 (SP2 skinning uses it) — no re-extraction needed later.
- `SkeletonStore` mirrors `NavMeshSystem::Instance()` (GameThread-write/render-read) but adds a mutex because its container mutates across loads while render reads; entries are immutable so `Get` can return a stable pointer.
- `SkeletonComponent` persists the raw stable-hash handle (correct because the handle is `AssetKeyHash`-derived, deterministic across runs — unlike the old mesh slot index). SP2 may switch to key-string via an extended `AssetRegistry` seam if readable `world.json` is wanted.
- SP2 (skinned vertex format + PSO + skinning shader + palette transport) and SP3 (`AnimationClip` + GameThread sampling + game-owned `AnimationComponent`) are separate specs. See [[project_id_stability]] for the asset-key reuse.
