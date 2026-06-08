# Animation SP3 — Single-clip playback — Design

**Date:** 2026-06-08
**Branch:** `feat/anim-clip-playback`
**Status:** Approved (brainstorm) — ready for implementation plan

## Context

Sub-project 3 of the animation effort (SP1 import+viz → SP2 skinning → **SP3 clip playback** →
SP4+ blend/IK; see [[project_animation]]). SP1+SP2 shipped (merged): `Skeleton`/`SkeletonStore`/
`SkeletonComponent` + bind-pose viz; GPU skinning (`SkinnedVertex` parallel bone buffer, weight
extraction, `PaletteFrame` atomic transport, GameThread skinning step `PublishPaletteFrame`, skinned
PSO + skinning VS + palette `StructuredBuffer @ t6`). SP2 drives the palette from the **bind pose**
(`ComputeBindPoseGlobals → ComputeSkinningPalette`), proven by a debug `SkinTest` bone-wiggle.

SP3 makes a skinned entity **play a real animation clip**: extract `aiAnimation` keyframes into an
`AnimationClip` asset, sample it per tick into per-bone globals, and feed those to the *existing*
`ComputeSkinningPalette` + palette transport + skinned PSO. **Zero render changes** — SP3 is GameThread
+ asset + ECS only. The `SkinTest` wiggle is removed (real playback supersedes it).

### As-built reuse points (verified)
- `PublishPaletteFrame(GameState&)` (`GameThread.cpp:703`) iterates skinned entities, does
  `globals = ComputeBindPoseGlobals(*sk)` (`:710`) → `ComputeSkinningPalette(*sk, globals)` (`:715`) →
  appends to a `PaletteFrame` published via `ApplicationContext::LatestPaletteFrame`. Called from
  `RunLoop:572` (before `PublishSnapshot`). SP3 swaps the `ComputeBindPoseGlobals` call for clip
  sampling and adds a time-advance pass; the publish + transport + GPU side are untouched.
- `ComputeSkinningPalette(const Skeleton&, const std::vector<glm::mat4>& globals)` (`Skinning.h`) =
  `globals[b] * inverseBind[b]` — reused verbatim.
- `SkeletonStore` (`src/engine/src/animation/SkeletonStore.{h,cpp}`) is the template for
  `AnimationStore` (singleton, `AssetKeyHash`-keyed, immutable + mutex). `SkeletonComponent` is the
  template for the new builtin `AnimationComponent`. The `WorkerThreadFunc` skeleton+skinning
  extraction (with its `boneNameToIndex` map) is where clip extraction hooks in.
- The test asset `assets/models/RiggedSimple.gltf` **has one animation** (confirmed) — SP3 plays it.
- assimp: `scene->mNumAnimations`/`mAnimations[]` → `aiAnimation { mName, mDuration, mTicksPerSecond,
  mNumChannels, mChannels[] }`; `aiNodeAnim { mNodeName, mNumPositionKeys/mPositionKeys[],
  mNumRotationKeys/mRotationKeys[], mNumScalingKeys/mScalingKeys[] }`; `aiVectorKey {mTime,mValue}`,
  `aiQuatKey {mTime, aiQuaternion mValue{w,x,y,z}}`.

## Goal

A skinned entity plays a single `AnimationClip`: keyframes sampled per tick → per-bone globals →
palette (reusing SP2). An engine-builtin `AnimationComponent` (clip + time + play state) drives it;
the engine advances time and samples; an editor picker assigns/plays a clip.

## Non-goals (later)

- No blending / transitions / state machine (SP4) — single clip only.
- No render-side pose interpolation — palette publishes at the 60 Hz tick; RenderThread reuses it
  (steps at 60 Hz, same as all ECS data). Smoothing is a deferred follow-up.
- No animation events, root motion, or IK (SP4+).
- No clip extraction via the editor file-load path (`MeshLoader`) — startup async path only (as SP2).
- No skinned shadows (`ShadowDepthPass` stays static).
- No `GAME_API_VERSION` bump (`GameState` unchanged). `ECS.h` X-macro changes → rebuild
  ecs+Engine+editor+game + editor restart.

## Components

### 1. `AnimationClip` asset + `SampleAnimation` (`src/common/include/AnimationClip.h`)
Immutable CPU data (glm + quaternion):
```cpp
#include <glm/gtc/quaternion.hpp>
struct AnimChannel {
    int boneIndex = -1;                                   // index into Skeleton::bones
    std::vector<std::pair<float, glm::vec3>> posKeys;     // (time seconds, value), ascending time
    std::vector<std::pair<float, glm::quat>> rotKeys;
    std::vector<std::pair<float, glm::vec3>> scaleKeys;
};
struct AnimationClip {
    std::string name;
    float duration = 0.0f;                               // seconds
    std::vector<AnimChannel> channels;                   // sparse: only animated bones
};
```
Pure sampler (reuses the topo order + `localBind` rest fallback):
```cpp
std::vector<glm::mat4> SampleAnimation(const Skeleton& sk, const AnimationClip& clip, float time);
```
Per bone: if a channel targets it, interpolate `pos` (lerp), `rot` (**slerp**, shortest-path), `scale`
(lerp) at `time` → `local = translate(pos) * mat4(rot) * scale(scale)`; else `local = bone.localBind`.
Then `global[b] = (parent<0) ? local : global[parent] * local`. Key interpolation: clamp before the
first key / after the last key to that key's value; between keys, find the bracketing pair (linear scan
or `std::lower_bound`; clips are small) and lerp/slerp by the normalized segment time. A small pure
helper per track type keeps it testable. Lives in `AnimationClip.h` (depends on `Skeleton.h` + glm);
caller pairs it with `ComputeSkinningPalette` from `Skinning.h`.

### 2. `AnimationStore` (engine singleton — mirror `SkeletonStore`)
`src/engine/src/animation/AnimationStore.{h,cpp}`: `handle → AnimationClip`, handle =
`AssetKeyHash("<modelKey>#anim/<clipName>")`. Append-only, immutable entries, `std::mutex`-guarded.
API: `uint64_t Add(const std::string& key, AnimationClip clip)` (de-dup by key), `const
AnimationClip* Get(uint64_t) const`, `std::string KeyForHandle(uint64_t) const`,
`std::vector<std::pair<uint64_t,std::string>> GetAssetList() const`. Engine-owned (CMake `# Animation`
block alongside `SkeletonStore.cpp`). GameThread-written (load) + GameThread-read (sampling).

### 3. `AnimationComponent` (engine builtin)
`src/common/include/ECS.h`:
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
+ `X(AnimationComponent)` in the X-macro; `to_json`/`from_json` (all 5 fields) in
`ComponentSerialization.h`; `r.Register<AnimationComponent>("AnimationComponent", true)` in
`ComponentSerializers.cpp`; typed branches in `ECSCommands.h` (`ApplyComponentCommand` +
`RemoveComponentByType`). Persisted (ClipId is a stable hash; Time/Speed/etc. round-trip).

### 4. Clip extraction (`GameThread::WorkerThreadFunc`)
After the SP1/SP2 skeleton+skinning block (which builds `boneNameToIndex`), and only when a skeleton
exists, extract clips:
- For each `scene->mAnimations[a]`: `name = mName` (fallback `"clip<a>"` if empty);
  `ticksPerSec = mTicksPerSecond != 0 ? mTicksPerSecond : 25.0`; `duration = mDuration / ticksPerSec`.
- For each `mChannels[c]` (`aiNodeAnim`): look up `mNodeName` in `boneNameToIndex`; skip if absent
  (channel targets a non-bone node). Build an `AnimChannel{boneIndex}` copying keys with
  `time = key.mTime / ticksPerSec`: pos/scale from `aiVectorKey`, rot from `aiQuatKey`
  (`glm::quat(w,x,y,z)` — note glm's ctor order vs assimp's `{w,x,y,z}`).
- Push the `AnimationClip` into `result.clips` (new `std::vector<AnimationClip> clips;` on
  `ModelLoadResult`) with its synth key `result.assetKey + "#anim/" + name`. CPU-side, **not** the GR
  ring (sampling is GameThread).

### 5. GameThread time-advance + sampling (extend `PublishPaletteFrame`)
`PublishPaletteFrame` gains a `float dt` param (the tick delta; passed from `RunLoop`). Before/within
the per-entity loop:
- **Advance** (mutates the master ECS): for each entity with `AnimationComponent`, if `Playing` and
  `ClipId` resolves, `Time += dt * Speed`; if `Looping` wrap `Time = fmod(Time, duration)` (guard
  `duration > 0`); else clamp `Time = min(Time, duration)` and set `Playing = false` at the end.
  (Use a mutating accessor — `Modify<AnimationComponent>` / `MutateArray` — GameThread owns the ECS.)
- **Sample:** in the skinned-entity loop, if the entity has an `AnimationComponent` with a resolved
  clip → `globals = SampleAnimation(*sk, *clip, anim.Time)`; else → `ComputeBindPoseGlobals(*sk)` (rest
  pose, as today). `palette = ComputeSkinningPalette(*sk, globals)` → append to the `PaletteFrame`
  exactly as SP2. Publish unchanged.
- **Remove the `SkinTest` path** (the flag in `DebugDrawSettings`, the panel checkbox, and the wiggle
  code) — real playback replaces it.

### 6. Editor — `AnimationEditor` picker
`src/editor/src/panels/inspector/AnimationEditor.{h,cpp}` (mirror `SkeletonEditor`): add/remove
`AnimationComponent`; a **clip dropdown** from `AnimationStore::Instance().GetAssetList()` (built lazily
inside `BeginCombo`, like the other pickers) setting `ClipId`; a **Play** checkbox (`Playing`), a
`Speed` `DragFloat`, a `Looping` checkbox; Apply via the typed `ModifyComponent` command. Registered in
`EcsInspectorPanel` + `editor/CMakeLists.txt`.

## Data flow

glTF → `WorkerThreadFunc` extracts `AnimationClip`(s) (+ skeleton + skinning) → `AnimationStore`
(GameThread drain). User assigns `AnimationComponent{ClipId, Playing=true}` via the editor. Per tick:
advance `Time` → `SampleAnimation(sk, clip, Time)` → globals → `ComputeSkinningPalette` →
`PaletteFrame` (SP2 transport) → skinned PSO (SP2) → animated deformation. 60 Hz tick; RenderThread
reuses the frame across render frames (deferred interpolation).

## Error handling / edge cases

- **`ClipId` unresolved** (AnimationStore miss) → fall back to `ComputeBindPoseGlobals` (rest) +
  `SM_WARN` once.
- **`AnimationComponent` without `SkeletonComponent`** → no skeleton to sample → the entity isn't in
  the skinned loop → renders static (skinned mesh w/o skeleton already handled by the SP2 sentinel:
  no palette range → VS skips skinning). `SM_WARN` once.
- **Channel targets a non-bone node** → skipped at extraction.
- **`time` before first / after last key** → clamp to that key's value (per track). **Empty track** →
  identity component (pos 0 / rot identity / scale 1).
- **`duration <= 0` or no channels** → bind pose.
- **Looping wrap** at `duration`; **non-looping** clamps + auto-stops (`Playing=false`).
- **Quat** slerp: shortest-path (negate one if `dot < 0`) + normalize; assimp `{w,x,y,z}` → glm
  `quat(w, x, y, z)`.
- **Paused** (`Playing=false`, `ClipId` set) → sample at the frozen `Time` (no advance).

## Testing

- **Unit (`tests/test_animation.cpp`):**
  - `SampleAnimation` on a synthetic 2-bone skeleton with a clip rotating bone 1 about Z from 0° (t=0)
    to 90° (t=1): assert bone-1 global at t=0 ≈ its `localBind`-derived rest, t=1 ≈ 90°, t=0.5 ≈ 45°
    (check a transformed point or the rotation of a basis vector). Bone 0 (no channel) stays at rest.
  - Key interpolation edges: `time` < first key → first value; > last key → last value.
  - Quat slerp midpoint correctness (45° between 0° and 90°).
  - (Time wrap `fmod(duration)` is in the GameThread step, not the pure sampler — covered by build +
    manual smoke; optionally a tiny pure `AdvanceClipTime(t,dt,speed,dur,loop)` helper to unit-test
    the wrap/clamp/auto-stop if it factors cleanly.)
- **Build-verified:** extraction, `AnimationStore`, the editor picker.
- **Manual smoke (human-owned):** assign RiggedSimple's clip + Play → the mesh **animates** (the
  bend cycles); pause → freezes at the current pose; Speed 2× → faster; Looping off → plays once and
  stops at the end pose; clearing the clip / removing the component → back to bind pose; static
  meshes unaffected.
- **Regression:** `test_animation`, `test_skinning`, `test_skeleton`, `test_assetkey`, `test_ecs`,
  `test_worldserial`, `test_compserial`, `test_reloadpreserve`, `test_playermove` green; full tree
  builds.

## Done criteria

- `AnimationClip` + `SampleAnimation` (lerp/slerp/scale, rest fallback, key-edge clamp); `test_animation`
  green.
- `AnimationStore` singleton; clips extracted from `aiAnimation` (bone-indexed via `boneNameToIndex`)
  + registered.
- `AnimationComponent` engine builtin (struct + X-macro + serializer + ECSCommands + register);
  round-trips in `world.json`.
- GameThread advances `Time` + samples the clip into the palette (reusing SP2 transport); `SkinTest`
  removed.
- `AnimationEditor` picker (clip + Play/Speed/Looping).
- RiggedSimple's clip plays on a skinned entity (manual smoke); full tree builds; suites green.
- `ECS.h` X-macro change ⇒ rebuild ecs+Engine+editor+game + restart; no `GAME_API_VERSION` bump.

## Notes

- SP3 reuses the *entire* SP2 GPU path (palette transport, skinned PSO/VS, `ComputeSkinningPalette`)
  unchanged — the only swap is `ComputeBindPoseGlobals` → `SampleAnimation` plus a time-advance pass.
- `AnimationComponent` is an engine builtin (engine owns the sampling machinery; the game writes
  `ClipId`/`Playing`/`Speed` to drive it) — consistent with `MeshComponent`/`SkeletonComponent`, and
  it lets the engine sample without naming a `Game.dll` type. (Deviates from the earlier roadmap note
  of "game-owned"; engine-builtin is simpler and sufficient for single-clip playback.)
- `AnimationStore` is GameThread-read (sampling), unlike `SkeletonStore` which is also RenderThread-read
  (debug viz); both are immutable engine singletons.
- SP4 (blending/state machine) layers on top: multiple `AnimationComponent`-like inputs + weighted
  pose blends feeding the same `ComputeSkinningPalette`; render-side interpolation is the separate
  deferred smoothness item.
