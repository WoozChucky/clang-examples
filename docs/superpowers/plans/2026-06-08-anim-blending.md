# Animation SP4 — Two-clip pose blending Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Blend two animation clips by a 0..1 weight into one pose (per-bone local TRS lerp/slerp) → palette (reusing SP2/SP3), driven by an extended `AnimationComponent` + editor slider, demonstrated on Fox Walk↔Run, with CesiumMan + Fox loading at scale (incl. textures).

**Architecture:** Six tasks: (1) sampler refactor — `BonePose` TRS + `SampleClipPose`/`BlendPoses`/`PoseToGlobals`/`DecomposeTRS`/`ComposeTRS`, `SampleAnimation` → wrapper, + tests; (2) `AnimationComponent` extension (ClipB/TimeB/BlendWeight, default-tolerant deser) + round-trip test; (3) GameThread blend branch; (4) humanoid assets + texture-path fix; (5) editor Clip B + weight; (6) regression. Blending happens in **local TRS** before the hierarchy walk; the SP2 palette transport + skinned PSO are unchanged.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-community`), assimp, glm (+ `gtc/quaternion`/`gtc/matrix_transform`), nlohmann/json, the SP2/SP3 animation pipeline.

**Scope:** Implements `docs/superpowers/specs/2026-06-08-anim-blending-design.md` (SP4 of the animation effort). `ECS.h` changes (AnimationComponent fields) ⇒ rebuild **ecs + Engine + editor + game** + editor restart. No `GAME_API_VERSION` bump. SP5 state-machine, SP6 IK, SP7 root-motion deferred.

> **Branch:** Work happens on `feat/anim-blending` (already created off `main`). Stay on it.

---

## Background facts (verified — locate edits by quoted code; line numbers approximate)

- `AnimationClip.h` current `SampleAnimation` (`:56-72`): builds per-bone `local` (animated = `translate(p)*mat4_cast(q)*scale(s)` via `anim_detail::SampleVec3/SampleQuat`; else `localBind`), then hierarchy walk `global[b] = parent<0?local:global[parent]*local`. Helpers `anim_detail::SampleVec3(keys,t,fallback)` / `SampleQuat(keys,t)` exist (clamp + lerp/slerp shortest-path). Includes already pull `glm`, `gtc/quaternion`, `gtc/matrix_transform`, `Skeleton.h`.
- `AnimationComponent { uint64 ClipId; float Time; float Speed; bool Looping; bool Playing; }` (`ECS.h` builtin). Serializer `to_json`/`from_json` (5 fields, `.at()`) in `ComponentSerialization.h`. Registered builtin; X-macro already has `X(AnimationComponent)` — **no X-macro change in SP4.**
- `GameThread::PublishPaletteFrame(GameState& state, float dt)` (`GameThread.cpp:709-746`) — the SP3 body:
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
                      if (a.Looping) { a.Time = std::fmod(a.Time, clip->duration); if (a.Time < 0.0f) a.Time += clip->duration; }
                      else if (a.Time >= clip->duration) { a.Time = clip->duration; a.Playing = false; }
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
- Worker texture load (`GameThread.cpp:873-881`, inside the `processMesh` lambda which captures `[&]` in `WorkerThreadFunc`, where `job` is in scope):
  ```cpp
                    if (material->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) {
                        std::string fullTexPath = std::string(texPath.C_Str());
                        std::vector<uint32_t> pixels; uint32_t width=0,height=0; std::string error;
                        if (!MaterialLoader::LoadMaterialFromFile(fullTexPath.c_str(), pixels, width, height, error)) {
                            SM_WARN("Failed to load material '%s': %s", fullTexPath.c_str(), error.c_str());
  ```
  `job.mtlBaseDir` = the model's directory (`entry.path().parent_path()` from the startup scan). `<filesystem>` is already included (used by the startup `directory_iterator`).
- `AnimationEditor.cpp` (SP3) has the Clip A dropdown (`AnimationStore::Instance().GetAssetList()` lazy in `BeginCombo`, sets `m_St.edit.ClipId`), Playing/Looping checkboxes, Speed/Time `DragFloat`, Apply button.
- The startup scan accepts `.gltf` (and `.obj`); CesiumMan/Fox are `.gltf`.

## Type/symbol contract (keep exact)

- `AnimationClip.h`: `struct BonePose { glm::vec3 T; glm::quat R; glm::vec3 S; };`, `glm::mat4 ComposeTRS(const BonePose&)`, `BonePose DecomposeTRS(const glm::mat4&)`, `std::vector<BonePose> SampleClipPose(const Skeleton&, const AnimationClip&, float)`, `std::vector<BonePose> BlendPoses(const std::vector<BonePose>&, const std::vector<BonePose>&, float)`, `std::vector<glm::mat4> PoseToGlobals(const Skeleton&, const std::vector<BonePose>&)`. `SampleAnimation` stays (now a wrapper).
- `AnimationComponent` += `uint64_t ClipB=0; float TimeB=0; float BlendWeight=0;`.

---

### Task 1: Sampler refactor — `BonePose` + blend functions + tests (TDD)

**Files:** Modify `src/common/include/AnimationClip.h`; Test `tests/test_blend.cpp` + `tests/CMakeLists.txt`.

- [ ] **Step 1: Write the failing test (`tests/test_blend.cpp`)**
```cpp
#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "AnimationClip.h"

static int g_Failures = 0;
#define EXPECT(cond) do { if(!(cond)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); ++g_Failures; } } while(0)
static bool nearf(float a, float b) { return std::fabs(a - b) < 1e-3f; }
static bool veq(const glm::vec3& a, const glm::vec3& b){ return nearf(a.x,b.x)&&nearf(a.y,b.y)&&nearf(a.z,b.z); }

static void T_decompose_roundtrip() {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(2,3,4))
                * glm::mat4_cast(glm::angleAxis(glm::radians(35.0f), glm::normalize(glm::vec3(0.2f,1,0.4f))))
                * glm::scale(glm::mat4(1.0f), glm::vec3(1.5f,2.0f,0.5f));
    BonePose p = DecomposeTRS(m);
    glm::mat4 r = ComposeTRS(p);
    glm::vec3 tp = glm::vec3(m * glm::vec4(1,1,1,1));
    glm::vec3 rp = glm::vec3(r * glm::vec4(1,1,1,1));
    EXPECT(veq(tp, rp)); // recomposed transform maps a test point the same
}

static void T_blend_endpoints_and_mid() {
    std::vector<BonePose> a(1), b(1);
    a[0].T = glm::vec3(0); a[0].R = glm::angleAxis(glm::radians(0.0f),  glm::vec3(0,0,1)); a[0].S = glm::vec3(1);
    b[0].T = glm::vec3(0,10,0); b[0].R = glm::angleAxis(glm::radians(90.0f), glm::vec3(0,0,1)); b[0].S = glm::vec3(3);
    EXPECT(veq(BlendPoses(a,b,0.0f)[0].T, a[0].T) && veq(BlendPoses(a,b,0.0f)[0].S, a[0].S));
    EXPECT(veq(BlendPoses(a,b,1.0f)[0].T, b[0].T) && veq(BlendPoses(a,b,1.0f)[0].S, b[0].S));
    BonePose mid = BlendPoses(a,b,0.5f)[0];
    EXPECT(veq(mid.T, glm::vec3(0,5,0)) && veq(mid.S, glm::vec3(2)));
    glm::vec3 rotated = mid.R * glm::vec3(1,0,0);     // 45deg about Z
    EXPECT(nearf(rotated.x, 0.70710678f) && nearf(rotated.y, 0.70710678f));
}

static void T_sample_equivalence() {
    // Same synthetic clip as SP3's test_animation: bone1 rotates 0->90deg about Z over [0,1].
    Skeleton sk; Bone root; root.name="root"; root.parent=-1; Bone child; child.name="child"; child.parent=0; sk.bones={root,child};
    AnimationClip clip; clip.name="spin"; clip.duration=1.0f;
    AnimChannel ch; ch.boneIndex=1;
    ch.posKeys={{0.0f,glm::vec3(0)},{1.0f,glm::vec3(0)}};
    ch.scaleKeys={{0.0f,glm::vec3(1)},{1.0f,glm::vec3(1)}};
    ch.rotKeys={{0.0f,glm::angleAxis(glm::radians(0.0f),glm::vec3(0,0,1))},{1.0f,glm::angleAxis(glm::radians(90.0f),glm::vec3(0,0,1))}};
    clip.channels={ch};
    auto g = PoseToGlobals(sk, SampleClipPose(sk, clip, 1.0f)); // via the new path
    glm::vec3 p = glm::vec3(g[1] * glm::vec4(1,0,0,1));
    EXPECT(nearf(p.x,0) && nearf(p.y,1));               // 90deg: (1,0,0)->(0,1,0)
    auto g2 = SampleAnimation(sk, clip, 0.5f);          // wrapper still works
    glm::vec3 p2 = glm::vec3(g2[1] * glm::vec4(1,0,0,1));
    EXPECT(nearf(p2.x,0.70710678f) && nearf(p2.y,0.70710678f));
}

int main() {
    T_decompose_roundtrip();
    T_blend_endpoints_and_mid();
    T_sample_equivalence();
    if (g_Failures == 0) std::printf("All blend tests passed.\n");
    return g_Failures ? 1 : 0;
}
```
Register `test_blend` in `tests/CMakeLists.txt` — copy the `test_animation` block verbatim, substituting `test_blend`/`test_blend.cpp`.

- [ ] **Step 2: Configure + build — confirm it FAILS**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_blend
```
Expected: COMPILE ERROR — `BonePose`/`DecomposeTRS`/`BlendPoses`/`PoseToGlobals`/`SampleClipPose` undeclared (TDD red).

- [ ] **Step 3: Refactor `AnimationClip.h`**
Add (after the `AnimationClip` struct, before/around the existing `SampleAnimation`; keep `anim_detail`):
```cpp
// One bone's LOCAL transform as TRS (so blending can slerp the rotation). Blend happens in this
// space, then ComposeTRS + the hierarchy walk (PoseToGlobals).
struct BonePose {
    glm::vec3 T{0.0f};
    glm::quat R{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 S{1.0f};
};

inline glm::mat4 ComposeTRS(const BonePose& p) {
    return glm::translate(glm::mat4(1.0f), p.T) * glm::mat4_cast(p.R) * glm::scale(glm::mat4(1.0f), p.S);
}

// Affine decompose (bind/clip locals are T*R*S, no skew): T = col3; S = column lengths; R = quat of
// the scale-normalized rotation 3x3. Degenerate (zero-length) columns fall back to an identity basis.
inline BonePose DecomposeTRS(const glm::mat4& m) {
    BonePose p;
    p.T = glm::vec3(m[3]);
    glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    p.S = glm::vec3(glm::length(c0), glm::length(c1), glm::length(c2));
    const glm::vec3 r0 = p.S.x > 1e-8f ? c0 / p.S.x : glm::vec3(1,0,0);
    const glm::vec3 r1 = p.S.y > 1e-8f ? c1 / p.S.y : glm::vec3(0,1,0);
    const glm::vec3 r2 = p.S.z > 1e-8f ? c2 / p.S.z : glm::vec3(0,0,1);
    p.R = glm::normalize(glm::quat_cast(glm::mat3(r0, r1, r2)));
    return p;
}

// Per-bone LOCAL pose at `time`: each bone starts at its rest (DecomposeTRS(localBind)); an animated
// bone overrides each track it has keys for (empty track keeps rest). Quat R is ready to slerp.
inline std::vector<BonePose> SampleClipPose(const Skeleton& sk, const AnimationClip& clip, float time) {
    std::vector<BonePose> pose(sk.bones.size());
    for (size_t b = 0; b < sk.bones.size(); ++b) pose[b] = DecomposeTRS(sk.bones[b].localBind);
    for (const auto& ch : clip.channels) {
        if (ch.boneIndex < 0 || static_cast<size_t>(ch.boneIndex) >= pose.size()) continue;
        BonePose& bp = pose[ch.boneIndex];
        if (!ch.posKeys.empty())   bp.T = anim_detail::SampleVec3(ch.posKeys,   time, bp.T);
        if (!ch.rotKeys.empty())   bp.R = anim_detail::SampleQuat(ch.rotKeys,   time);
        if (!ch.scaleKeys.empty()) bp.S = anim_detail::SampleVec3(ch.scaleKeys, time, bp.S);
    }
    return pose;
}

// Per-bone blend of two LOCAL poses: lerp T/S, slerp R (shortest-path). Clamped to the shorter size.
inline std::vector<BonePose> BlendPoses(const std::vector<BonePose>& a, const std::vector<BonePose>& b, float w) {
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    std::vector<BonePose> out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i].T = glm::mix(a[i].T, b[i].T, w);
        glm::quat qb = b[i].R;
        if (glm::dot(a[i].R, qb) < 0.0f) qb = -qb;
        out[i].R = glm::normalize(glm::slerp(a[i].R, qb, w));
        out[i].S = glm::mix(a[i].S, b[i].S, w);
    }
    return out;
}

// Compose each LOCAL TRS + hierarchy walk (topo order: parent index < b) -> model-space globals.
inline std::vector<glm::mat4> PoseToGlobals(const Skeleton& sk, const std::vector<BonePose>& localPoses) {
    std::vector<glm::mat4> global(sk.bones.size());
    for (size_t b = 0; b < sk.bones.size() && b < localPoses.size(); ++b) {
        const glm::mat4 local = ComposeTRS(localPoses[b]);
        const int parent = sk.bones[b].parent;
        global[b] = (parent < 0) ? local : global[parent] * local;
    }
    return global;
}
```
REPLACE the existing `SampleAnimation` body with the wrapper:
```cpp
// Single-clip globals (SP3 convenience): sample the pose, then walk. Equivalent to the prior direct
// implementation for fully-keyed clips.
inline std::vector<glm::mat4> SampleAnimation(const Skeleton& sk, const AnimationClip& clip, float time) {
    return PoseToGlobals(sk, SampleClipPose(sk, clip, time));
}
```
(`glm::quat_cast` is in `<glm/gtc/quaternion.hpp>` — already included. `glm::length`/`glm::mix` are core.)

- [ ] **Step 4: Build + run — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community --target test_blend
./out/build/msvc-win64-vs2026-community/bin/Debug/test_blend.exe
```
Expected: `All blend tests passed.`

- [ ] **Step 5: Commit**
```
git -C C:/dev/clang-examples add src/common/include/AnimationClip.h tests/test_blend.cpp tests/CMakeLists.txt
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): pose-TRS sampler refactor (BonePose/SampleClipPose/BlendPoses/PoseToGlobals) + blend tests"
```

---

### Task 2: `AnimationComponent` blend fields + round-trip test (TDD)

**Files:** `src/common/include/ECS.h`, `src/common/include/ComponentSerialization.h`, `tests/test_worldserial.cpp`.

- [ ] **Step 1: Write the failing test (`tests/test_worldserial.cpp`)**
Add after `T14_animation_roundtrip()`:
```cpp
static void T15_animation_blend_roundtrip()
{
    AnimationComponent in; in.ClipId=0xAA; in.Time=0.5f; in.Speed=1.5f; in.Looping=false; in.Playing=true;
    in.ClipB=0xBB; in.TimeB=0.25f; in.BlendWeight=0.7f;
    const nlohmann::json j = in;
    const auto out = j.get<AnimationComponent>();
    EXPECT(out.ClipB == in.ClipB);
    EXPECT(out.TimeB == in.TimeB);
    EXPECT(out.BlendWeight == in.BlendWeight);

    // Backward-compat: an OLD-shape json (no blend fields) must load with blend defaults, not throw.
    nlohmann::json old = { {"ClipId", 5u}, {"Time", 1.0f}, {"Speed", 1.0f}, {"Looping", true}, {"Playing", false} };
    const auto migrated = old.get<AnimationComponent>();
    EXPECT(migrated.ClipId == 5u && migrated.ClipB == 0ull && migrated.BlendWeight == 0.0f);
}
```
Register `    T15_animation_blend_roundtrip();` in `main()` after the `T14_animation_roundtrip();` line.

- [ ] **Step 2: Build the test — confirm it FAILS**
```
cmake --build --preset msvc-win64-vs2026-community --target test_worldserial
```
Expected: FAIL — `in.ClipB`/etc. no member (compile error). TDD red.

- [ ] **Step 3: Extend the struct (`ECS.h`)**
In `struct AnimationComponent`, after `bool Playing = false;` add:
```cpp
    uint64_t ClipB       = 0;     // second clip for blending; 0 = no blend (pure ClipId)
    float    TimeB       = 0.0f;  // B cursor
    float    BlendWeight = 0.0f;  // 0 = A, 1 = B
```

- [ ] **Step 4: Serializer (`ComponentSerialization.h`)** — extend the AnimationComponent (de)serializers. `to_json` writes all 8; `from_json` uses `value()` (default-tolerant) for the THREE new fields so old saves load:
```cpp
inline void to_json(nlohmann::json& j, const AnimationComponent& t) {
    j = nlohmann::json{ {"ClipId", t.ClipId}, {"Time", t.Time}, {"Speed", t.Speed}, {"Looping", t.Looping}, {"Playing", t.Playing},
                        {"ClipB", t.ClipB}, {"TimeB", t.TimeB}, {"BlendWeight", t.BlendWeight} };
}
inline void from_json(const nlohmann::json& j, AnimationComponent& t) {
    j.at("ClipId").get_to(t.ClipId);
    j.at("Time").get_to(t.Time);
    j.at("Speed").get_to(t.Speed);
    j.at("Looping").get_to(t.Looping);
    j.at("Playing").get_to(t.Playing);
    t.ClipB       = j.value("ClipB", uint64_t{0});
    t.TimeB       = j.value("TimeB", 0.0f);
    t.BlendWeight = j.value("BlendWeight", 0.0f);
}
```

- [ ] **Step 5: Build + run — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community --target ecs --target test_worldserial
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
```
Expected: `All world-serialization tests passed.`

- [ ] **Step 6: Commit**
```
git -C C:/dev/clang-examples add src/common/include/ECS.h src/common/include/ComponentSerialization.h tests/test_worldserial.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(ecs): AnimationComponent ClipB/TimeB/BlendWeight (additive, default-tolerant deser) + test"
```

---

### Task 3: GameThread blend branch (`PublishPaletteFrame`)

**Files:** `src/engine/src/threading/GameThread.cpp`.

- [ ] **Step 1: Replace the `if (anim && clip) { ... } else { ... }` block** in `PublishPaletteFrame` with the blend-aware version (resolves clip B, advances both cursors, blends in local space):
```cpp
        const AnimationComponent* anim = state.World.GetComponent<AnimationComponent>(e);
        const AnimationClip* clipA = (anim && anim->ClipId) ? AnimationStore::Instance().Get(anim->ClipId) : nullptr;
        const AnimationClip* clipB = (anim && anim->ClipB)  ? AnimationStore::Instance().Get(anim->ClipB)  : nullptr;
        if (anim && clipA) {
            float tA = anim->Time, tB = anim->TimeB;
            float w  = anim->BlendWeight;
            state.World.Modify<AnimationComponent>(e, [&](AnimationComponent& a) {
                auto advance = [&](float& time, const AnimationClip* c) {
                    if (a.Playing && c && c->duration > 0.0f) {
                        time += dt * a.Speed;
                        if (a.Looping) { time = std::fmod(time, c->duration); if (time < 0.0f) time += c->duration; }
                        else if (time >= c->duration) { time = c->duration; }
                    }
                };
                advance(a.Time,  clipA);
                advance(a.TimeB, clipB);
                if (!a.Looping && a.Playing && clipA->duration > 0.0f && a.Time >= clipA->duration) a.Playing = false;
                tA = a.Time; tB = a.TimeB; w = glm::clamp(a.BlendWeight, 0.0f, 1.0f);
            });
            if (clipB) {
                globals = PoseToGlobals(*sk, BlendPoses(SampleClipPose(*sk, *clipA, tA),
                                                        SampleClipPose(*sk, *clipB, tB), w));
            } else {
                globals = SampleAnimation(*sk, *clipA, tA);
            }
        } else {
            if (anim && anim->ClipId && !clipA)
                SM_WARN("AnimationComponent on entity %llu: clip A handle %llu not in AnimationStore", (unsigned long long)e, (unsigned long long)anim->ClipId);
            globals = ComputeBindPoseGlobals(*sk);
        }
```
(`glm::clamp` is in `<glm/glm.hpp>` / `glm/common.hpp` — already included via the glm headers used here; if unresolved add `#include <glm/common.hpp>`. `SampleClipPose`/`BlendPoses`/`PoseToGlobals` come via `AnimationClip.h`, already included. Keep the surrounding `std::vector<glm::mat4> globals;` declaration + the palette-append code unchanged.)

- [ ] **Step 2: Build full tree — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: clean. (Single-clip path: `clipB` null → `SampleAnimation` as SP3; blend path active when ClipB set.)

- [ ] **Step 3: Commit**
```
git -C C:/dev/clang-examples add src/engine/src/threading/GameThread.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): GameThread blends clip A+B in local pose space (BlendWeight) into the palette"
```

---

### Task 4: Humanoid assets (CesiumMan + Fox) + texture-path fix

**Files:** `assets/models/CesiumMan.*`, `assets/models/Fox.*` (new); `src/engine/src/threading/GameThread.cpp`.

- [ ] **Step 1: Fetch CesiumMan + Fox — URI-DRIVEN (do NOT rename referenced files)**
The glTF references its buffer + image by relative `uri`; assimp resolves those against the `.gltf`'s
directory, so each referenced file MUST sit next to the `.gltf` under its EXACT uri name. Base URL:
`https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/<Model>/glTF/<file>`.

1. Fetch the two `.gltf` first:
```
curl -L -o C:/dev/clang-examples/assets/models/CesiumMan.gltf https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/CesiumMan/glTF/CesiumMan.gltf
curl -L -o C:/dev/clang-examples/assets/models/Fox.gltf        https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Fox/glTF/Fox.gltf
```
2. For EACH `.gltf`, read its `"buffers"[].uri` and `"images"[].uri` (grep the file for `"uri"`). For
every distinct uri `U`, fetch it to `assets/models/U` under the SAME name (no renaming):
```
curl -L -o C:/dev/clang-examples/assets/models/<U> https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/<Model>/glTF/<U>
```
(Typical: CesiumMan → `CesiumMan.bin` + `CesiumMan.jpg`; Fox → `Fox.bin` + `Texture.png` — but TRUST the
uris you read, not this list.) Verify each file non-empty + right type (gltf = JSON starting `{`;
bin/png/jpg = binary). Confirm `git status --short assets/models/` lists all fetched files untracked
(not gitignored). Record the exact filenames you fetched — you'll stage those exact paths in Step 4.

- [ ] **Step 2: Texture-path resolution fix (`GameThread.cpp` `processMesh`)**
Change the diffuse-texture load to resolve a relative uri against the model directory (`job.mtlBaseDir`). Replace:
```cpp
                        std::string fullTexPath = std::string(texPath.C_Str());
```
with:
```cpp
                        std::string fullTexPath = std::string(texPath.C_Str());
                        {
                            std::filesystem::path tp(fullTexPath);
                            if (tp.is_relative() && !job.mtlBaseDir.empty())
                                fullTexPath = (std::filesystem::path(job.mtlBaseDir) / tp).string();
                        }
```
(`<filesystem>` already included. `job` is captured by the `[&]` `processMesh` lambda — confirm by reading the lambda capture; if it captures something else, use the variable that holds the model dir. The startup scan passes `entry.path().parent_path().string()` as `mtlBaseDir`.)

- [ ] **Step 3: Build — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community --target Engine --target game
```
Expected: clean.

- [ ] **Step 4: Commit**
`git -C C:/dev/clang-examples status` — confirm the new `assets/models/CesiumMan.*` + `Fox.*` files are present + intended; NO `world.json`/settings json.
```
git -C C:/dev/clang-examples add src/engine/src/threading/GameThread.cpp
git -C C:/dev/clang-examples add <each exact asset path you fetched in Step 1: the two .gltf + every referenced .bin/.png/.jpg>
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): add CesiumMan + Fox humanoid assets; resolve relative diffuse-texture path against model dir"
```
(Stage the EXACT filenames from Step 1 — the gltf + its referenced uris. Verify with `show --stat HEAD`.)

---

### Task 5: `AnimationEditor` — Clip B + Blend Weight

**Files:** `src/editor/src/panels/inspector/AnimationEditor.cpp`.

- [ ] **Step 1: Add the Clip B dropdown + weight slider** to `DrawEditor`, after the existing Clip A combo (mirror it for `m_St.edit.ClipB`) and after the Speed/Time drags:
```cpp
    // Clip B (blend target)
    std::string currentB = AnimationStore::Instance().KeyForHandle(m_St.edit.ClipB);
    if (currentB.empty()) currentB = "(none)";
    if (ImGui::BeginCombo("Clip B", currentB.c_str())) {
        const auto clips = AnimationStore::Instance().GetAssetList();
        if (clips.empty()) ImGui::TextDisabled("No clips loaded");
        for (const auto& [handle, key] : clips) {
            const bool isSelected = (handle == m_St.edit.ClipB);
            const char* label = key.empty() ? "(none)" : key.c_str();
            if (ImGui::Selectable(label, isSelected)) { m_St.edit.ClipB = handle; m_St.modified = true; }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::SliderFloat("Blend Weight", &m_St.edit.BlendWeight, 0.0f, 1.0f)) m_St.modified = true;
    if (ImGui::DragFloat("Time B", &m_St.edit.TimeB, 0.01f, 0.0f, 1000.0f)) m_St.modified = true;
```
(Place these BEFORE the `if (m_St.modified) ...` / Apply-button block so they're part of the editor body. `SliderFloat`/`DragFloat` are standard ImGui.)

- [ ] **Step 2: Build editor — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```

- [ ] **Step 3: Commit**
```
git -C C:/dev/clang-examples add src/editor/src/panels/inspector/AnimationEditor.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(editor): AnimationEditor Clip B + Blend Weight slider"
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
./out/build/msvc-win64-vs2026-community/bin/Debug/test_blend.exe
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
1. **Fox loads at scale:** console shows skeleton (many bones) + skinning + 3 clips (`Fox.gltf#anim/Survey|Walk|Run`) extracted; assign Fox's mesh + skeleton to an entity; its **texture shows** (not magenta). Assign Animation Component, Clip A = `Fox.gltf#anim/Walk`, Playing → walks.
2. **Blend:** Clip B = `Fox.gltf#anim/Run`; slide **Blend Weight** 0→1 → motion blends Walk→Run smoothly; mid-weight is a believable mix.
3. **CesiumMan:** loads + plays its walk; texture shows.
4. **Regression:** RiggedSimple single-clip still plays (ClipB=0); static `.obj`s unaffected; an entity with skeleton + no anim → bind pose.

- [ ] **Step 4: Commit fixups (only if needed).**

---

## Done criteria

- `BonePose` + `DecomposeTRS`/`ComposeTRS`/`SampleClipPose`/`BlendPoses`/`PoseToGlobals`; `SampleAnimation` is a wrapper; `test_blend` green (decompose round-trip, blend endpoints/mid, sampler equivalence).
- `AnimationComponent` has `ClipB`/`TimeB`/`BlendWeight` (additive, default-tolerant deser); `test_worldserial` T15 green incl. old-shape load.
- GameThread blends A+B in local pose space (weight-clamped) into the palette; single-clip + bind-pose paths intact.
- CesiumMan + Fox committed + load (mesh+skeleton+skin+**texture**) at scale; relative texture path resolved against the model dir.
- `AnimationEditor` Clip B + Blend Weight; Fox Walk↔Run blends on the slider (manual smoke).
- Full tree builds; all suites green; no `GAME_API_VERSION` bump.

## Notes

- Blending in **local TRS** (slerp R) then `PoseToGlobals` is the correct order — `BonePose` carries a quat for this reason. The refactor keeps `SampleAnimation` as a single-clip wrapper, so SP3 behavior is preserved (verified by the equivalence test); SP5 (state machine) builds on `BlendPoses`.
- `SampleClipPose` improves on SP3's direct sampler: an empty per-track falls back to the bone's REST component (decomposed `localBind`) instead of 0/identity/1 — behavior-identical for fully-keyed assimp clips, more correct for partial channels.
- Independent A/B time cursors are an SP4 simplification; **phase-sync (gait matching) is SP5**.
- Textured humanoids exercise the diffuse-texture load for the first time — the relative-path fix (join with `job.mtlBaseDir`) is expected.
