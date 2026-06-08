# Animation SP3 — Single-clip playback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A skinned entity plays a single animation clip — `aiAnimation` keyframes sampled per tick into per-bone globals → palette (reusing the SP2 transport + skinned PSO unchanged), driven by an engine-builtin `AnimationComponent` and an editor picker.

**Architecture:** Seven tasks: (1) `AnimationClip.h` + pure `SampleAnimation` + tests; (2) `AnimationStore` singleton (mirror `SkeletonStore`); (3) `AnimationComponent` engine builtin + round-trip test; (4) clip extraction in `WorkerThreadFunc` + registration; (5) GameThread time-advance + sampling (extend `PublishPaletteFrame`, +dt) + remove `SkinTest`; (6) `AnimationEditor` picker; (7) regression. SP3 changes **no render code** — it swaps `ComputeBindPoseGlobals` for `SampleAnimation` and adds a time-advance pass.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-community`), assimp, glm (+ `gtc/quaternion`), nlohmann/json, the SP2 palette transport (`ApplicationContext::LatestPaletteFrame`) + skinned PSO.

**Scope:** Implements `docs/superpowers/specs/2026-06-08-anim-clip-playback-design.md` (SP3 of the animation effort). `ECS.h` X-macro changes ⇒ rebuild **ecs + Engine + editor + game** + editor restart. No `GAME_API_VERSION` bump. Humanoid asset deferred to SP4 (stays on RiggedSimple).

> **Branch:** Work happens on `feat/anim-clip-playback` (already created off `main`). Stay on it.

---

## Background facts (verified — locate edits by quoted code; line numbers approximate)

- `PublishPaletteFrame(GameState& state)` (`GameThread.cpp:703-721`) — current SP2 body:
  ```cpp
  void GameThread::PublishPaletteFrame(GameState& state) {
      auto frame = std::make_shared<PaletteFrame>();
      const bool wiggle = GetDebugDrawSettings().SkinTest;
      const float t = static_cast<float>(TimeNowSec());
      state.World.Each<SkeletonComponent>([&](EntityId e, const SkeletonComponent& sc) {
          const Skeleton* sk = SkeletonStore::Instance().Get(sc.SkeletonId);
          if (!sk || sk->bones.empty()) return;
          std::vector<glm::mat4> globals = ComputeBindPoseGlobals(*sk);
          if (wiggle && !globals.empty()) {
              const size_t b = globals.size() - 1;
              globals[b] = globals[b] * glm::rotate(glm::mat4(1.0f), std::sin(t) * 0.8f, glm::vec3(0,0,1));
          }
          const std::vector<glm::mat4> palette = ComputeSkinningPalette(*sk, globals);
          const uint32_t offset = static_cast<uint32_t>(frame->matrices.size());
          frame->matrices.insert(frame->matrices.end(), palette.begin(), palette.end());
          frame->ranges.push_back(PaletteFrame::Range{ e, offset, static_cast<uint32_t>(palette.size()) });
      });
      m_AppContext->LatestPaletteFrame.store(std::move(frame), std::memory_order_release);
  }
  ```
  Declared `GameThread.h:34` `void PublishPaletteFrame(GameState& state);`. Called `RunLoop:572` `PublishPaletteFrame(gameState);` (before `PublishSnapshot` at :573). The per-tick clamped delta is `gameState.DeltaTime` (set `RunLoop:525`).
- `SkinTest` lives in exactly 3 places: `RenderStats.h:28` (`bool SkinTest = false;`), `RenderStatsPanel.cpp:44` (the checkbox), `GameThread.cpp:705` (the `wiggle` read). All removed in Task 5.
- `state.World` is the master GameThread ECS (mutable). `Modify<T>(EntityId, lambda)` mutates one entity's component (used in the GameThread response drain, e.g. `gameState.World.Modify<MeshComponent>(id, [&](auto& m){...})`). `GetComponent<T>(EntityId)` → `const T*`. `Each<T>(cb(EntityId, const T&))` iterates; the variadic overload supports one or more component types.
- SP2 reuse: `ComputeSkinningPalette(const Skeleton&, const std::vector<glm::mat4>&)` (`Skinning.h`); `PaletteFrame`/`LatestPaletteFrame` transport; skinned PSO consumes it — all unchanged.
- `SkeletonStore.{h,cpp}` (`src/engine/src/animation/`) is the exact template for `AnimationStore` (singleton `Instance()`, `AssetKeyHash` key, `Add`/`Get`/`KeyForHandle`/`GetAssetList`, `std::mutex`, immutable entries). `SkeletonComponent` (`ECS.h`, X-macro, `ComponentSerialization.h`, `ComponentSerializers.cpp` register, `ECSCommands.h` 2 branches) is the template for `AnimationComponent`. `SkeletonEditor.{h,cpp}` + its `EcsInspectorPanel`/`editor/CMakeLists.txt` registration is the template for `AnimationEditor`.
- X-macro tail (`ECS.h`) currently ends `X(SkeletonComponent)` (last, no trailing backslash). `ComponentSerializers.cpp` builtin block ends `r.Register<SkeletonComponent>("SkeletonComponent", true);`.
- `ModelLoadResult` (`GameThread.h`) has SP1/SP2 fields incl. `Skeleton skeleton; bool hasSkeleton; std::string skeletonKey; std::vector<SkinnedVertex> skinning;`. The `WorkerThreadFunc` skeleton/skinning block builds `boneNameToIndex` (`std::unordered_map<std::string,int>`) inside `if (!boneInverseBind.empty()) { ... }`.
- assimp: `scene->mNumAnimations`/`mAnimations[a]` → `aiAnimation { aiString mName; double mDuration; double mTicksPerSecond; unsigned mNumChannels; aiNodeAnim** mChannels; }`. `aiNodeAnim { aiString mNodeName; unsigned mNumPositionKeys; aiVectorKey* mPositionKeys; (rotation: aiQuatKey* mRotationKeys; scaling: aiVectorKey* mScalingKeys) }`. `aiVectorKey { double mTime; aiVector3D mValue; }`, `aiQuatKey { double mTime; aiQuaternion mValue; }` (`mValue.w/x/y/z`).

## Type/symbol contract (keep exact)

- `src/common/include/AnimationClip.h`: `AnimChannel { int boneIndex; vector<pair<float,glm::vec3>> posKeys; vector<pair<float,glm::quat>> rotKeys; vector<pair<float,glm::vec3>> scaleKeys; }`, `AnimationClip { std::string name; float duration; std::vector<AnimChannel> channels; }`, `std::vector<glm::mat4> SampleAnimation(const Skeleton&, const AnimationClip&, float time)`.
- `src/engine/src/animation/AnimationStore.{h,cpp}`: singleton, `uint64_t Add(const std::string&, AnimationClip)`, `const AnimationClip* Get(uint64_t) const`, `std::string KeyForHandle(uint64_t) const`, `std::vector<std::pair<uint64_t,std::string>> GetAssetList() const`.
- `AnimationComponent { uint64_t ClipId=0; float Time=0; float Speed=1; bool Looping=true; bool Playing=false; }`; key `"AnimationComponent"`.
- `ModelLoadResult.clips` : `std::vector<AnimationClip>`; each clip's key = `result.assetKey + "#anim/" + name`.
- `PublishPaletteFrame(GameState& state, float dt)`.

---

### Task 1: `AnimationClip.h` + `SampleAnimation` + unit tests (TDD)

**Files:** Create `src/common/include/AnimationClip.h`; Test `tests/test_animation.cpp` + `tests/CMakeLists.txt`.

- [ ] **Step 1: Write the failing test (`tests/test_animation.cpp`)**
```cpp
#include <cstdio>
#include <cmath>
#include <vector>
#include <utility>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "AnimationClip.h"

static int g_Failures = 0;
#define EXPECT(cond) do { if(!(cond)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); ++g_Failures; } } while(0)
static bool nearf(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static void T_sample_rotation() {
    // 2-bone skeleton, identity binds. Bone 1 channel rotates about +Z from 0deg (t=0) to 90deg (t=1).
    Skeleton sk;
    Bone root;  root.name="root";  root.parent=-1; // localBind identity
    Bone child; child.name="child"; child.parent=0; // localBind identity
    sk.bones = { root, child };

    AnimationClip clip; clip.name="spin"; clip.duration=1.0f;
    AnimChannel ch; ch.boneIndex = 1;
    ch.posKeys   = { {0.0f, glm::vec3(0)}, {1.0f, glm::vec3(0)} };
    ch.scaleKeys = { {0.0f, glm::vec3(1)}, {1.0f, glm::vec3(1)} };
    ch.rotKeys   = { {0.0f, glm::angleAxis(glm::radians(0.0f),  glm::vec3(0,0,1))},
                     {1.0f, glm::angleAxis(glm::radians(90.0f), glm::vec3(0,0,1))} };
    clip.channels = { ch };

    auto pt = [&](float t){ auto g = SampleAnimation(sk, clip, t); return glm::vec3(g[1] * glm::vec4(1,0,0,1)); };
    glm::vec3 p0 = pt(0.0f), ph = pt(0.5f), p1 = pt(1.0f);
    EXPECT(nearf(p0.x,1) && nearf(p0.y,0));                 // 0deg: (1,0,0)
    EXPECT(nearf(p1.x,0) && nearf(p1.y,1));                 // 90deg: (0,1,0)
    EXPECT(nearf(ph.x,0.70710678f) && nearf(ph.y,0.70710678f)); // 45deg
    // Clamp beyond ends:
    glm::vec3 pAfter = pt(5.0f);
    EXPECT(nearf(pAfter.x,0) && nearf(pAfter.y,1));         // clamps to last key (90deg)
    // Unanimated bone 0 stays at rest (identity):
    auto g = SampleAnimation(sk, clip, 0.5f);
    glm::vec3 r = glm::vec3(g[0] * glm::vec4(1,0,0,1));
    EXPECT(nearf(r.x,1) && nearf(r.y,0) && nearf(r.z,0));
}

int main() {
    T_sample_rotation();
    if (g_Failures == 0) std::printf("All animation tests passed.\n");
    return g_Failures ? 1 : 0;
}
```
Register `test_animation` in `tests/CMakeLists.txt` — copy the `test_skinning` block verbatim, substituting `test_animation`/`test_animation.cpp` (same `CommonHeaders glm::glm ecs` libs + include dir + GLM defines + `RUNTIME_OUTPUT_DIRECTORY/FOLDER Tests`).

- [ ] **Step 2: Configure + build — confirm it FAILS**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_animation
```
Expected: COMPILE ERROR — `AnimationClip.h` not found / `SampleAnimation` undeclared (TDD red).

- [ ] **Step 3: Create `src/common/include/AnimationClip.h`**
```cpp
#pragma once
#include <vector>
#include <string>
#include <utility>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "Skeleton.h"

// Immutable animation-clip asset (CPU). Per-bone keyframe tracks; channels are sparse (only animated
// bones). Times are in seconds. See docs/superpowers/specs/2026-06-08-anim-clip-playback-design.md.
struct AnimChannel {
    int boneIndex = -1;                                   // index into Skeleton::bones
    std::vector<std::pair<float, glm::vec3>> posKeys;     // (time, value), ascending time
    std::vector<std::pair<float, glm::quat>> rotKeys;
    std::vector<std::pair<float, glm::vec3>> scaleKeys;
};
struct AnimationClip {
    std::string name;
    float duration = 0.0f;                               // seconds
    std::vector<AnimChannel> channels;                   // sparse
};

namespace anim_detail {
    inline glm::vec3 SampleVec3(const std::vector<std::pair<float,glm::vec3>>& k, float t, glm::vec3 fallback) {
        if (k.empty()) return fallback;
        if (t <= k.front().first) return k.front().second;
        if (t >= k.back().first)  return k.back().second;
        for (size_t i = 1; i < k.size(); ++i) {
            if (t < k[i].first) {
                const float u = (t - k[i-1].first) / (k[i].first - k[i-1].first);
                return glm::mix(k[i-1].second, k[i].second, u);
            }
        }
        return k.back().second;
    }
    inline glm::quat SampleQuat(const std::vector<std::pair<float,glm::quat>>& k, float t) {
        if (k.empty()) return glm::quat(1,0,0,0);
        if (t <= k.front().first) return k.front().second;
        if (t >= k.back().first)  return k.back().second;
        for (size_t i = 1; i < k.size(); ++i) {
            if (t < k[i].first) {
                const float u = (t - k[i-1].first) / (k[i].first - k[i-1].first);
                glm::quat a = k[i-1].second, b = k[i].second;
                if (glm::dot(a, b) < 0.0f) b = -b;        // shortest path
                return glm::normalize(glm::slerp(a, b, u));
            }
        }
        return k.back().second;
    }
}

// Per-bone model-space globals at `time`: animated bones use their channel's sampled T*R*S (empty
// track => neutral component: pos 0 / rot identity / scale 1 — assimp clips populate all three);
// unanimated bones use localBind (rest). Hierarchy walk requires topo order (parent index < b).
inline std::vector<glm::mat4> SampleAnimation(const Skeleton& sk, const AnimationClip& clip, float time) {
    std::vector<glm::mat4> local(sk.bones.size());
    for (size_t b = 0; b < sk.bones.size(); ++b) local[b] = sk.bones[b].localBind;
    for (const auto& ch : clip.channels) {
        if (ch.boneIndex < 0 || static_cast<size_t>(ch.boneIndex) >= local.size()) continue;
        const glm::vec3 p = anim_detail::SampleVec3(ch.posKeys,   time, glm::vec3(0.0f));
        const glm::quat q = anim_detail::SampleQuat(ch.rotKeys,   time);
        const glm::vec3 s = anim_detail::SampleVec3(ch.scaleKeys, time, glm::vec3(1.0f));
        local[ch.boneIndex] = glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(q) * glm::scale(glm::mat4(1.0f), s);
    }
    std::vector<glm::mat4> global(sk.bones.size());
    for (size_t b = 0; b < sk.bones.size(); ++b) {
        const int parent = sk.bones[b].parent;
        global[b] = (parent < 0) ? local[b] : global[parent] * local[b];
    }
    return global;
}
```

- [ ] **Step 4: Build + run — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community --target test_animation
./out/build/msvc-win64-vs2026-community/bin/Debug/test_animation.exe
```
Expected: `All animation tests passed.`

- [ ] **Step 5: Commit**
```
git -C C:/dev/clang-examples add src/common/include/AnimationClip.h tests/test_animation.cpp tests/CMakeLists.txt
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): AnimationClip + SampleAnimation (lerp/slerp keyframe sampling) + tests"
```

---

### Task 2: `AnimationStore` singleton (engine)

**Files:** Create `src/engine/src/animation/AnimationStore.{h,cpp}`; Modify `src/engine/CMakeLists.txt`.

Build-verified. **Mirror `src/engine/src/animation/SkeletonStore.{h,cpp}` EXACTLY**, substituting `Skeleton`→`AnimationClip`, `SkeletonStore`→`AnimationStore`, `#include "Skeleton.h"`→`#include "AnimationClip.h"`, and `m_ByHandle`'s `Entry{ key, skeleton }`→`Entry{ key, clip }`.

- [ ] **Step 1: Create `src/engine/src/animation/AnimationStore.h`**
```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <mutex>
#include "Engine.h"          // ENGINE_API
#include "AnimationClip.h"

// Process-wide store of immutable AnimationClip assets, keyed by a stable hash handle
// (AssetKeyHash of "<modelKey>#anim/<name>"). GameThread-written (Add at load) + GameThread-read
// (sampling). Entries immutable once added; the container mutates across loads so a mutex guards
// access. Mirrors SkeletonStore.
class ENGINE_API AnimationStore {
public:
    static AnimationStore& Instance();
    uint64_t Add(const std::string& key, AnimationClip clip);   // de-dup by key
    const AnimationClip* Get(uint64_t handle) const;            // null if unknown
    std::string KeyForHandle(uint64_t handle) const;
    std::vector<std::pair<uint64_t, std::string>> GetAssetList() const;
private:
    AnimationStore() = default;
    struct Entry { std::string key; AnimationClip clip; };
    mutable std::mutex m_Mutex;
    std::unordered_map<uint64_t, Entry> m_ByHandle;
};
```

- [ ] **Step 2: Create `src/engine/src/animation/AnimationStore.cpp`** (mirror SkeletonStore.cpp)
```cpp
#include "animation/AnimationStore.h"
#include "AssetKey.h"

AnimationStore& AnimationStore::Instance() { static AnimationStore s; return s; }

uint64_t AnimationStore::Add(const std::string& key, AnimationClip clip) {
    const uint64_t handle = AssetKeyHash(key);
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    if (it != m_ByHandle.end()) return handle;
    m_ByHandle.emplace(handle, Entry{ key, std::move(clip) });
    return handle;
}
const AnimationClip* AnimationStore::Get(uint64_t handle) const {
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    return it == m_ByHandle.end() ? nullptr : &it->second.clip;
}
std::string AnimationStore::KeyForHandle(uint64_t handle) const {
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    return it == m_ByHandle.end() ? std::string() : it->second.key;
}
std::vector<std::pair<uint64_t, std::string>> AnimationStore::GetAssetList() const {
    std::scoped_lock lk(m_Mutex);
    std::vector<std::pair<uint64_t, std::string>> out;
    out.reserve(m_ByHandle.size());
    for (const auto& [h, e] : m_ByHandle) out.emplace_back(h, e.key);
    return out;
}
```
(NOTE: the returned `const AnimationClip*` from `Get` stays valid for process lifetime — entries never erased/mutated, `unordered_map` nodes are stable across rehash — same contract as SkeletonStore.)

- [ ] **Step 3: Add to `src/engine/CMakeLists.txt`** — in the `# Animation` block next to `src/animation/SkeletonStore.cpp` add `src/animation/AnimationStore.cpp`.

- [ ] **Step 4: Build `Engine` — GREEN**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target Engine
```

- [ ] **Step 5: Commit**
```
git -C C:/dev/clang-examples add src/engine/src/animation/AnimationStore.h src/engine/src/animation/AnimationStore.cpp src/engine/CMakeLists.txt
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): AnimationStore singleton (immutable, handle-keyed, mutex-guarded)"
```

---

### Task 3: `AnimationComponent` engine builtin + round-trip test (TDD)

**Files:** `src/common/include/ECS.h`, `src/common/include/ComponentSerialization.h`, `src/ecs/src/ComponentSerializers.cpp`, `src/common/include/ECSCommands.h`, `tests/test_worldserial.cpp`.

Mirror the `SkeletonComponent` builtin wiring exactly.

- [ ] **Step 1: Write the failing test (`tests/test_worldserial.cpp`)**
Add after `T13_skeleton_roundtrip()`:
```cpp
static void T14_animation_roundtrip()
{
    AnimationComponent in; in.ClipId = 0x00CAFE0042u; in.Time = 1.25f; in.Speed = 2.0f; in.Looping = false; in.Playing = true;
    const nlohmann::json j = in;
    const auto out = j.get<AnimationComponent>();
    EXPECT(out.ClipId == in.ClipId);
    EXPECT(out.Time == in.Time);
    EXPECT(out.Speed == in.Speed);
    EXPECT(out.Looping == in.Looping);
    EXPECT(out.Playing == in.Playing);

    AnimationComponent def;
    EXPECT(def.ClipId == 0ull && def.Looping == true && def.Playing == false);
}
```
Register `    T14_animation_roundtrip();` in `main()` after the `T13_skeleton_roundtrip();` line.

- [ ] **Step 2: Build the test — confirm it FAILS**
```
cmake --build --preset msvc-win64-vs2026-community --target test_worldserial
```
Expected: COMPILE ERROR — `AnimationComponent` undeclared (TDD red).

- [ ] **Step 3: Struct + X-macro (`ECS.h`)**
After the `struct SkeletonComponent { ... };` add:
```cpp

// Plays an AnimationClip (in AnimationStore) on a skinned entity. Engine builtin: the engine advances
// Time + samples in the GameThread skinning step; the game sets ClipId/Playing/Speed to drive it.
struct AnimationComponent {
    uint64_t ClipId  = 0;     // AnimationStore handle (stable hash); 0 = none
    float    Time    = 0.0f;  // playback cursor, seconds
    float    Speed   = 1.0f;
    bool     Looping = true;
    bool     Playing = false;
};
```
Change the X-macro tail `    X(SkeletonComponent)` to:
```cpp
    X(SkeletonComponent) \
    X(AnimationComponent)
```

- [ ] **Step 4: Serializer (`ComponentSerialization.h`)**
```cpp
inline void to_json(nlohmann::json& j, const AnimationComponent& t) {
    j = nlohmann::json{ {"ClipId", t.ClipId}, {"Time", t.Time}, {"Speed", t.Speed}, {"Looping", t.Looping}, {"Playing", t.Playing} };
}
inline void from_json(const nlohmann::json& j, AnimationComponent& t) {
    j.at("ClipId").get_to(t.ClipId);
    j.at("Time").get_to(t.Time);
    j.at("Speed").get_to(t.Speed);
    j.at("Looping").get_to(t.Looping);
    j.at("Playing").get_to(t.Playing);
}
```

- [ ] **Step 5: Register builtin (`ComponentSerializers.cpp`)** — after `r.Register<SkeletonComponent>("SkeletonComponent", true);` add:
```cpp
        r.Register<AnimationComponent>("AnimationComponent", true);
```

- [ ] **Step 6: Typed command branches (`ECSCommands.h`)** — mirror the `SkeletonComponent` branches.
In `ApplyComponentCommand`, after the SkeletonComponent branch:
```cpp
        } else if (componentData.Type == std::type_index(typeid(AnimationComponent))) {
            if (auto* a = componentData.Get<AnimationComponent>()) {
                world.AddComponent(entity, *a);
            }
```
In `RemoveComponentByType`, after the SkeletonComponent branch:
```cpp
        } else if (typeIndex == std::type_index(typeid(AnimationComponent))) {
            world.RemoveComponent<AnimationComponent>(entity);
```

- [ ] **Step 7: Build + run — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community --target ecs --target test_worldserial
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
```
Expected: `All world-serialization tests passed.`

- [ ] **Step 8: Commit**
```
git -C C:/dev/clang-examples add src/common/include/ECS.h src/common/include/ComponentSerialization.h src/ecs/src/ComponentSerializers.cpp src/common/include/ECSCommands.h tests/test_worldserial.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(ecs): AnimationComponent builtin (clip + time + play state) + round-trip test"
```

---

### Task 4: Clip extraction + registration (`WorkerThreadFunc` + drain)

**Files:** `src/engine/src/threading/GameThread.h`, `src/engine/src/threading/GameThread.cpp`.

- [ ] **Step 1: `ModelLoadResult.clips` (`GameThread.h`)**
Add `#include "AnimationClip.h"` with the other includes. In `struct ModelLoadResult`, after `std::vector<SkinnedVertex> skinning;` add:
```cpp
        std::vector<AnimationClip> clips; // animation clips extracted from the scene (skeleton-bone-indexed)
```

- [ ] **Step 2: Extract clips in `WorkerThreadFunc` (`GameThread.cpp`)**
Add `#include "AnimationClip.h"` at the top. Inside the SP1/SP2 skeleton block, AFTER the skinning extraction (`result.skinning[...] = MakeSkinnedVertex(...)` loop) and still inside the `if (!skel.bones.empty()) { ... }` scope (so `boneNameToIndex` is available), add:
```cpp
                    // --- Animation clip extraction (SP3) ---
                    for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
                        const aiAnimation* anim = scene->mAnimations[a];
                        const double tps = (anim->mTicksPerSecond != 0.0) ? anim->mTicksPerSecond : 25.0;
                        AnimationClip clip;
                        clip.name     = anim->mName.length ? anim->mName.C_Str() : ("clip" + std::to_string(a));
                        clip.duration = static_cast<float>(anim->mDuration / tps);
                        for (unsigned c = 0; c < anim->mNumChannels; ++c) {
                            const aiNodeAnim* nodeAnim = anim->mChannels[c];
                            auto ni = boneNameToIndex.find(nodeAnim->mNodeName.C_Str());
                            if (ni == boneNameToIndex.end()) continue; // channel targets a non-bone node
                            AnimChannel ch;
                            ch.boneIndex = ni->second;
                            ch.posKeys.reserve(nodeAnim->mNumPositionKeys);
                            for (unsigned k = 0; k < nodeAnim->mNumPositionKeys; ++k) {
                                const aiVectorKey& vk = nodeAnim->mPositionKeys[k];
                                ch.posKeys.emplace_back(static_cast<float>(vk.mTime / tps), glm::vec3(vk.mValue.x, vk.mValue.y, vk.mValue.z));
                            }
                            ch.rotKeys.reserve(nodeAnim->mNumRotationKeys);
                            for (unsigned k = 0; k < nodeAnim->mNumRotationKeys; ++k) {
                                const aiQuatKey& qk = nodeAnim->mRotationKeys[k];
                                ch.rotKeys.emplace_back(static_cast<float>(qk.mTime / tps), glm::quat(qk.mValue.w, qk.mValue.x, qk.mValue.y, qk.mValue.z));
                            }
                            ch.scaleKeys.reserve(nodeAnim->mNumScalingKeys);
                            for (unsigned k = 0; k < nodeAnim->mNumScalingKeys; ++k) {
                                const aiVectorKey& sk2 = nodeAnim->mScalingKeys[k];
                                ch.scaleKeys.emplace_back(static_cast<float>(sk2.mTime / tps), glm::vec3(sk2.mValue.x, sk2.mValue.y, sk2.mValue.z));
                            }
                            clip.channels.push_back(std::move(ch));
                        }
                        result.clips.push_back(std::move(clip));
                        SM_TRACE("Animation extracted: '%s#anim/%s' (%.2fs, %zu channels)",
                                 result.assetKey.c_str(), result.clips.back().name.c_str(),
                                 result.clips.back().duration, result.clips.back().channels.size());
                    }
```
(`glm::quat(w,x,y,z)` — glm's ctor is w-first, matching assimp `aiQuaternion{w,x,y,z}`. `<glm/gtc/quaternion.hpp>` comes via `AnimationClip.h`.)

- [ ] **Step 3: Register clips in the completed-jobs drain (`GameThread.cpp`)**
Add `#include "animation/AnimationStore.h"`. In the drain, next to the existing skeleton registration (`if (res.hasSkeleton) { SkeletonStore::Instance().Add(...); ... }`), add:
```cpp
                    for (auto& clip : res.clips) {
                        AnimationStore::Instance().Add(res.assetKey + "#anim/" + clip.name, clip);
                    }
```
(Registers each clip by its `<modelKey>#anim/<name>` key — idempotent de-dup. CPU-side, no ring.)

- [ ] **Step 4: Build full tree — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: clean. (Clips extracted + registered; nothing samples them yet → no visual change.)

- [ ] **Step 5: Commit**
```
git -C C:/dev/clang-examples add src/engine/src/threading/GameThread.h src/engine/src/threading/GameThread.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): extract aiAnimation clips (bone-indexed) + register in AnimationStore"
```

---

### Task 5: GameThread time-advance + sampling + remove SkinTest

**Files:** `src/engine/src/threading/GameThread.{h,cpp}`, `src/engine/src/rendering/RenderStats.h`, `src/editor/src/panels/RenderStatsPanel.cpp`.

- [ ] **Step 1: `PublishPaletteFrame` gains `dt` (`GameThread.h` + call site)**
`GameThread.h:34`: change to `void PublishPaletteFrame(GameState& state, float dt);`.
`GameThread.cpp` `RunLoop:572`: change `PublishPaletteFrame(gameState);` to:
```cpp
			PublishPaletteFrame(gameState, static_cast<float>(gameState.DeltaTime));
```

- [ ] **Step 2: Rewrite `PublishPaletteFrame` (`GameThread.cpp`)** — replace the whole SP2 body:
```cpp
void GameThread::PublishPaletteFrame(GameState& state, float dt) {
    auto frame = std::make_shared<PaletteFrame>();
    state.World.Each<SkeletonComponent>([&](EntityId e, const SkeletonComponent& sc) {
        const Skeleton* sk = SkeletonStore::Instance().Get(sc.SkeletonId);
        if (!sk || sk->bones.empty()) return;

        std::vector<glm::mat4> globals;
        const AnimationComponent* anim = state.World.GetComponent<AnimationComponent>(e);
        const AnimationClip* clip = (anim && anim->ClipId) ? AnimationStore::Instance().Get(anim->ClipId) : nullptr;
        if (anim && clip) {
            float sampleTime = anim->Time;
            state.World.Modify<AnimationComponent>(e, [&](AnimationComponent& a) {
                if (a.Playing && clip->duration > 0.0f) {
                    a.Time += dt * a.Speed;
                    if (a.Looping) {
                        a.Time = std::fmod(a.Time, clip->duration);
                        if (a.Time < 0.0f) a.Time += clip->duration;
                    } else if (a.Time >= clip->duration) {
                        a.Time = clip->duration;
                        a.Playing = false;
                    }
                }
                sampleTime = a.Time;
            });
            globals = SampleAnimation(*sk, *clip, sampleTime);
        } else {
            if (anim && anim->ClipId && !clip)
                SM_WARN("AnimationComponent on entity %llu: clip handle %llu not in AnimationStore", (unsigned long long)e, (unsigned long long)anim->ClipId);
            globals = ComputeBindPoseGlobals(*sk);
        }

        const std::vector<glm::mat4> palette = ComputeSkinningPalette(*sk, globals);
        const uint32_t offset = static_cast<uint32_t>(frame->matrices.size());
        frame->matrices.insert(frame->matrices.end(), palette.begin(), palette.end());
        frame->ranges.push_back(PaletteFrame::Range{ e, offset, static_cast<uint32_t>(palette.size()) });
    });
    m_AppContext->LatestPaletteFrame.store(std::move(frame), std::memory_order_release);
}
```
Add includes if missing: `#include "AnimationClip.h"`, `#include "animation/AnimationStore.h"`, `#include <cmath>` (for `std::fmod` — likely already present). The `SM_WARN` here fires every tick for an unresolved clip; if that's too noisy, gate it behind a `static thread_local std::unordered_set<EntityId>` warn-once — optional, plan-author's discretion (the warn-once pattern is used elsewhere). Minimal acceptable: leave the per-tick warn (it only fires on a genuine misconfiguration).

- [ ] **Step 3: Remove `SkinTest`**
- `RenderStats.h:28`: delete the `bool SkinTest = false;` line.
- `RenderStatsPanel.cpp:44`: delete the `changed |= ImGui::Checkbox("Skin Test (wiggle)", &dd.SkinTest);` line.
- `GameThread.cpp`: the wiggle code is already gone (replaced by Step 2). If `TimeNowSec()`/`glm::rotate`/`std::sin` became unused in this function, that's fine — they're used elsewhere in the file; do not remove their includes.
Confirm `git grep SkinTest` returns nothing under `src/`.

- [ ] **Step 4: Build full tree — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: clean. (A skinned entity with an `AnimationComponent`+clip now animates; bind pose otherwise.)

- [ ] **Step 5: Commit**
```
git -C C:/dev/clang-examples add src/engine/src/threading/GameThread.h src/engine/src/threading/GameThread.cpp src/engine/src/rendering/RenderStats.h src/editor/src/panels/RenderStatsPanel.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): GameThread advances + samples AnimationComponent into the palette; remove SkinTest"
```

---

### Task 6: `AnimationEditor` inspector picker

**Files:** Create `src/editor/src/panels/inspector/AnimationEditor.{h,cpp}`; Modify `src/editor/src/panels/EcsInspectorPanel.cpp`, `src/editor/CMakeLists.txt`.

Mirror `SkeletonEditor.{h,cpp}` (clip dropdown lazily inside `BeginCombo`, like all pickers). Build-verified.

- [ ] **Step 1: `AnimationEditor.h`** (mirror SkeletonEditor.h):
```cpp
#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class AnimationEditor final : public IComponentEditor {
    EditState<AnimationComponent> m_St;
public:
    const char* Label() const override { return "Animation Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<AnimationComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
```

- [ ] **Step 2: `AnimationEditor.cpp`**
```cpp
#include "AnimationEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <utility>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "animation/AnimationStore.h"
#include "lib.h"

void AnimationEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, AnimationComponent{});
    if (!ctx.App->ECSCommandRing.Push(addCmd))
        SM_WARN("ECS command queue full! Add component command dropped.");
}
void AnimationEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<AnimationComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd))
        SM_WARN("ECS command queue full! Remove component command dropped.");
}
void AnimationEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    if (!m_St.Begin(ctx, e)) return;

    std::string current = AnimationStore::Instance().KeyForHandle(m_St.edit.ClipId);
    if (current.empty()) current = "(none)";
    if (ImGui::BeginCombo("Clip", current.c_str())) {
        const auto clips = AnimationStore::Instance().GetAssetList(); // built only while open
        if (clips.empty()) ImGui::TextDisabled("No clips loaded");
        for (const auto& [handle, key] : clips) {
            const bool isSelected = (handle == m_St.edit.ClipId);
            const char* label = key.empty() ? "(none)" : key.c_str();
            if (ImGui::Selectable(label, isSelected)) { m_St.edit.ClipId = handle; m_St.modified = true; }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::Checkbox("Playing", &m_St.edit.Playing)) m_St.modified = true;
    if (ImGui::Checkbox("Looping", &m_St.edit.Looping)) m_St.modified = true;
    if (ImGui::DragFloat("Speed", &m_St.edit.Speed, 0.05f, 0.0f, 8.0f)) m_St.modified = true;
    if (ImGui::DragFloat("Time", &m_St.edit.Time, 0.01f, 0.0f, 1000.0f)) m_St.modified = true;

    ImGui::Spacing();
    if (m_St.modified)
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "* Modified (not yet saved)");
    if (ImGui::Button("Apply Changes##Animation", ImVec2(150, 0))) {
        ECSCommand modifyCmd = ECSCommand::ModifyComponent(e, m_St.edit);
        if (!ctx.App->ECSCommandRing.Push(modifyCmd))
            SM_WARN("ECS command queue full! Modify command dropped.");
        m_St.modified = false;
    }
}
```
NOTE: `EditState<T>` "live-refreshes edit from snapshot while not modified" — so the `Time` field will tick forward in the inspector as the clip plays (the engine advances it). That's fine (read-only-ish display); edits while `modified` are preserved until Apply.

- [ ] **Step 3: Register (`EcsInspectorPanel.cpp` + `editor/CMakeLists.txt`)**
`EcsInspectorPanel.cpp`: add `#include "inspector/AnimationEditor.h"` (after the SkeletonEditor include) and `m_Editors.push_back(std::make_unique<AnimationEditor>());` (after the SkeletonEditor registration).
`editor/CMakeLists.txt`: add `src/panels/inspector/AnimationEditor.cpp` (after `SkeletonEditor.cpp`).

- [ ] **Step 4: Build editor — GREEN**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target editor
```

- [ ] **Step 5: Commit**
```
git -C C:/dev/clang-examples add src/editor/src/panels/inspector/AnimationEditor.h src/editor/src/panels/inspector/AnimationEditor.cpp src/editor/src/panels/EcsInspectorPanel.cpp src/editor/CMakeLists.txt
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(editor): AnimationEditor picker (clip + Play/Speed/Looping/Time)"
```

---

### Task 7: Full regression + manual smoke

**Files:** none (verification; fixups only if needed).

- [ ] **Step 1: Reconfigure + full clean build**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community
```
Expected: all targets build, no errors / `LNK`.

- [ ] **Step 2: Run the suites**
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_animation.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_skinning.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_skeleton.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_assetkey.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_compserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_reloadpreserve.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_playermove.exe
```
Expected: each prints its pass line (`test_compserial` `NopeNotReal` warn + `test_reloadpreserve` `BadComp` warn expected).

- [ ] **Step 3: Manual smoke (human-owned; editor restart — `ECS.h` changed)**
With RiggedSimple on a skinned entity (Mesh + Skeleton assigned, as in SP2):
1. Add an **Animation Component**; in the **Clip** dropdown pick `models/RiggedSimple.gltf#anim/<name>`; check **Playing**; Apply.
2. The mesh **animates** (the clip cycles); the `Time` field ticks in the inspector.
3. Uncheck **Playing** → freezes at the current pose. **Speed** 2× → faster. **Looping** off → plays once, stops at the end pose (`Playing` auto-clears).
4. Remove the Animation Component (or clear the clip) → back to bind pose.
5. A skinned entity with no Animation Component → bind pose (unchanged). Static meshes unaffected.

- [ ] **Step 4: Commit fixups (only if needed).**

---

## Done criteria

- `AnimationClip` + `SampleAnimation` (lerp/slerp/scale, rest fallback, key-edge clamp); `test_animation` green.
- `AnimationStore` singleton; clips extracted from `aiAnimation` (bone-indexed via `boneNameToIndex`) + registered.
- `AnimationComponent` builtin (struct + X-macro + serializer + ECSCommands + register); `test_worldserial` T14 green.
- GameThread advances `Time` + samples the clip into the palette (reusing SP2 transport); `SkinTest` fully removed (`git grep SkinTest` empty).
- `AnimationEditor` picker (clip + Play/Speed/Looping/Time).
- RiggedSimple's clip plays on a skinned entity (manual smoke); full tree builds; all suites green.
- No render-code changes; no `GAME_API_VERSION` bump; `ECS.h` X-macro change ⇒ rebuild ecs+Engine+editor+game + restart.

## Notes

- SP3 reuses the SP2 GPU path entirely — the only runtime change is `ComputeBindPoseGlobals` → `SampleAnimation` + the time-advance pass.
- `SampleAnimation` assumes assimp channels populate all three tracks (they do); a missing track falls back to a neutral component (pos 0 / rot identity / scale 1), and a bone with NO channel uses its full `localBind` rest.
- `AnimationComponent.Time` is mutated on the master ECS in the skinning step (GameThread owns it), via `Modify` — runs before `PublishSnapshot`, so the snapshot the RenderThread reads has the advanced time.
- Render-side interpolation for >60 FPS smoothness is deferred (see [[project_animation]]); SP4 adds blending + owns the humanoid asset.
