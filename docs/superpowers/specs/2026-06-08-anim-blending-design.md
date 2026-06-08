# Animation SP4 — Two-clip pose blending (+ humanoid assets) — Design

**Date:** 2026-06-08
**Branch:** `feat/anim-blending`
**Status:** Approved (brainstorm) — ready for implementation plan

## Context

The animation effort's "blending → state machine → IK → root-motion" bucket decomposes into separate
sub-projects (see [[project_animation]]): **SP4 two-clip blending** (this) → SP5 state machine →
SP6 IK → SP7 root motion. SP1-3 shipped (merged): skeleton import + bind-pose viz; GPU skinning
(`SkinnedVertex`, `PaletteFrame` atomic transport, `PublishPaletteFrame`, skinned PSO + palette SB);
single-clip playback (`AnimationClip`/`AnimationStore`, engine-builtin `AnimationComponent`,
`SampleAnimation` lerp/slerp, GameThread sample). Everything has ridden on **RiggedSimple** (2 bones,
1 clip).

SP4 adds **weighted blending of two clips** and brings in the multi-clip **humanoids** (CesiumMan +
Fox), which also validates the SP1-3 pipeline at real scale (many bones, multi-mesh, textures, larger
palette). Fox (Survey/Walk/Run) is the blend demo (Walk↔Run); CesiumMan exercises a textured humanoid.

### As-built reuse points (verified)
- `SampleAnimation(const Skeleton&, const AnimationClip&, float time) → std::vector<glm::mat4>`
  (`AnimationClip.h`) internally samples per-bone local TRS (lerp pos/scale, slerp rot, rest =
  `localBind`) then does the hierarchy walk. SP4 splits this into a pose stage + a walk stage so two
  poses can be blended in **local** space before the walk.
- GameThread `PublishPaletteFrame(GameState&, float dt)` (`GameThread.cpp`): per skinned entity →
  `globals = SampleAnimation(...) | ComputeBindPoseGlobals(...)` → `ComputeSkinningPalette` → publish.
  SP4 adds a blend branch here. Transport + skinned PSO unchanged.
- `AnimationComponent { uint64 ClipId; float Time; float Speed; bool Looping; bool Playing; }`
  (`ECS.h` builtin) — SP4 extends it.
- `AnimationStore` + `AnimationEditor` (clip dropdown) — SP4 adds a second dropdown + weight slider.
- **Texture path is untested:** the worker's `aiTextureType_DIFFUSE` → `MaterialLoader::LoadMaterialFromFile`
  path (`GameThread.cpp` `processMesh`) has never run (RiggedSimple is untextured). Textured humanoids
  exercise it for the first time; the texture uri is relative and may need joining to `job.mtlBaseDir`.

## Goal

Blend two `AnimationClip`s by a 0..1 weight into one pose (per-bone local TRS lerp/slerp) → palette,
driven by an extended `AnimationComponent` + editor weight slider, demonstrated on Fox Walk↔Run, with
CesiumMan + Fox loading correctly at scale (incl. textures).

## Non-goals (later)

- No N-way blend trees / additive layers — two-clip linear blend only.
- No animation state machine / transitions / crossfades (SP5).
- No phase-synchronization between the two clips (independent time cursors; gait matching is SP5).
- No IK (SP6), no root motion (SP7), no render-side interpolation.
- No `GAME_API_VERSION` bump. `ECS.h` change (AnimationComponent fields) → rebuild
  ecs+Engine+editor+game + editor restart.

## Components

### 1. Pose representation + sampler refactor (`AnimationClip.h`)
Blending must occur in **local TRS** (lerp T/S, **slerp** R), then compose + walk — blending composed
or global matrices is incorrect. Refactor the SP3 sampler:
```cpp
struct BonePose { glm::vec3 T{0.0f}; glm::quat R{1,0,0,0}; glm::vec3 S{1.0f}; }; // one bone's local transform

// Affine decompose (no skew — bind/clip locals are T*R*S): T = col3, S = column lengths, R = quat of
// the scale-normalized 3x3.
glm::mat4 ComposeTRS(const BonePose&);
BonePose  DecomposeTRS(const glm::mat4&);

// Per-bone LOCAL pose at `time`: animated bone -> sampled (T, slerp R, S); else DecomposeTRS(localBind).
std::vector<BonePose> SampleClipPose(const Skeleton&, const AnimationClip&, float time);

// Per-bone blend: mix(T), slerp(R, shortest-path), mix(S). Sizes must match (same skeleton).
std::vector<BonePose> BlendPoses(const std::vector<BonePose>& a, const std::vector<BonePose>& b, float w);

// Compose each local TRS + hierarchy walk (parent index < b) -> model-space globals.
std::vector<glm::mat4> PoseToGlobals(const Skeleton&, const std::vector<BonePose>& localPoses);
```
`SampleAnimation` is retained as a thin wrapper `PoseToGlobals(sk, SampleClipPose(sk, clip, time))`
(SP3 single-clip path semantically unchanged). The per-track key interpolation helpers
(`SampleVec3`/`SampleQuat`) are reused; `SampleClipPose` produces a `BonePose` (the rot already a quat,
ready to slerp) instead of immediately composing a matrix.

### 2. `AnimationComponent` extended (additive — single-clip preserved)
```cpp
struct AnimationComponent {
    uint64_t ClipId      = 0;     // clip A (existing); 0 = none
    float    Time        = 0.0f;  // A cursor (existing)
    float    Speed       = 1.0f;
    bool     Looping     = true;
    bool     Playing     = false;
    uint64_t ClipB       = 0;     // clip B; 0 = no blend (pure A = SP3 behavior)
    float    TimeB       = 0.0f;  // B cursor
    float    BlendWeight = 0.0f;  // 0 = A, 1 = B
};
```
`to_json`/`from_json` gain the three new fields. `ClipB==0` → SP3 single-clip; world.json with the old
3-field shape still loads if `from_json` tolerates missing keys (use `value(key, default)` for the new
fields so older saves don't throw) — or accept the breaking change (early-dev) and re-save. Plan picks
`value()`-with-default for the new fields (cheap, avoids breaking existing scenes).

### 3. GameThread blend (extend `PublishPaletteFrame`)
In the per-entity skinning branch, after resolving clip A (and advancing `Time`):
- Resolve `clipB = (anim->ClipB ? AnimationStore::Get(anim->ClipB) : nullptr)`. Advance `TimeB`
  (same loop/clamp rules, by `Speed`, wrapped by `clipB->duration`) via the same `Modify`.
- If `clipB`: `globals = PoseToGlobals(sk, BlendPoses(SampleClipPose(sk,*clipA,TimeA),
  SampleClipPose(sk,*clipB,TimeB), clamp(BlendWeight,0,1)))`.
- Else: SP3 single-clip path (`PoseToGlobals(sk, SampleClipPose(sk,*clipA,TimeA))`), or bind pose if no
  A. `ComputeSkinningPalette` + publish unchanged.

### 4. Humanoid assets (both)
Fetch into `assets/models/` from the Khronos glTF-Sample-Assets repo (commit each + referenced files):
- **CesiumMan** (`CesiumMan.gltf` + `.bin` + its texture) — textured humanoid, has a walk clip.
- **Fox** (`Fox.gltf` + `.bin` + `Texture.png`) — Survey/Walk/Run clips (the blend demo).
Both `.gltf` reference external uris (`.bin`, image); fetch every referenced file. `.gitattributes`
already marks `*.bin`/`*.png`/`*.jpg` binary.

### 5. Texture-path resolution (fix if the humanoids surface it)
The worker loads a material's diffuse texture via `MaterialLoader::LoadMaterialFromFile(texPath, ...)`
where `texPath` is assimp's (relative) uri. With a textured model this likely fails (CWD ≠ model dir).
Fix: resolve `texPath` against the model's directory (`job.mtlBaseDir`, already passed to the worker) —
e.g. if `texPath` is relative, prepend `mtlBaseDir + "/"`. Gated on the humanoid checkpoint surfacing
it; if textures already load (assimp may absolutize), no change. `SM_WARN` on a failed texture load
(already present) — keep.

### 6. Editor
`AnimationEditor` gains: a **Clip B** dropdown (from `AnimationStore::GetAssetList()`, lazy in
`BeginCombo`), a **Blend Weight** `DragFloat`/`SliderFloat` (0..1), and a **Time B** display. Existing
Clip A + Play/Speed/Looping retained. Pick Fox#Walk as A, Fox#Run as B, slide weight → blended motion.

## Data flow

Fox/CesiumMan glTF → extract mesh + skeleton + skinning + clips (SP1-3 pipeline) + texture →
MeshSystem/SkeletonStore/AnimationStore/MaterialSystem. Assign `AnimationComponent{ClipId=Walk,
ClipB=Run, Playing, BlendWeight}`. Per tick: advance TimeA/TimeB → `SampleClipPose` ×2 →
`BlendPoses(w)` → `PoseToGlobals` → `ComputeSkinningPalette` → `PaletteFrame` (SP2) → skinned PSO →
blended deformation. 60 Hz tick, RenderThread reuses (deferred interpolation).

## Error handling / edge cases

- **`ClipB==0`** → single-clip (SP3), no blend, no extra sampling.
- **`ClipB` set but unresolved** (store miss) → fall back to single-clip A + `SM_WARN` once.
- **Bone animated in A but not B** (or vice-versa) → the non-animating side's `SampleClipPose` uses
  `DecomposeTRS(localBind)` (rest) for that bone, so blending degrades to rest — correct.
- **`BlendPoses` size mismatch** → both poses come from the same `SkeletonComponent`'s skeleton, so
  sizes match; defensively clamp the loop to `min(a.size(), b.size())`.
- **`BlendWeight`** clamped to [0,1].
- **Quat blend** shortest-path (negate if `dot<0`) + normalize (as in SP3 `SampleQuat`).
- **`DecomposeTRS` of a zero/degenerate scale** → guard against divide-by-zero (zero-length column →
  identity rotation, zero scale).
- **Texture load failure** (missing/relative path) → `SM_WARN` + the existing missing-material
  fallback (magenta) — non-fatal; mesh still loads + animates.
- **Old `world.json`** AnimationComponent (3 fields) → `from_json` uses `value(key, default)` for
  ClipB/TimeB/BlendWeight → loads as single-clip.

## Testing

- **Unit (`tests/test_blend.cpp`):**
  - `DecomposeTRS`/`ComposeTRS` round-trip an affine `translate*rotate*scale` matrix (≈ original).
  - `BlendPoses`: at w=0 → equals A; w=1 → equals B; w=0.5 → T/S are midpoints, R is the slerp midpoint
    of two known rotations (e.g. 0° and 90° → 45°).
  - Refactor-equivalence: `PoseToGlobals(sk, SampleClipPose(sk,clip,t))` equals the SP3
    `SampleAnimation` result for the synthetic 2-bone clip (so the refactor preserves single-clip
    behavior).
- **Build-verified:** the GameThread blend branch + editor.
- **Manual smoke (human-owned):**
  - Fox loads (mesh + skeleton + skin + **texture** visible); assign Walk as A → plays.
  - Assign Run as B, slide **Blend Weight** 0→1 → motion blends smoothly Walk→Run; mid-weight is a
    believable mix.
  - CesiumMan loads + plays its walk (texture shows).
  - RiggedSimple still works (single-clip, ClipB=0); static meshes unaffected.
- **Regression:** `test_blend`, `test_animation`, `test_skinning`, `test_skeleton`, `test_assetkey`,
  `test_ecs`, `test_worldserial`, `test_compserial`, `test_reloadpreserve`, `test_playermove` green;
  full tree builds.

## Done criteria

- `BonePose` + `SampleClipPose`/`BlendPoses`/`PoseToGlobals`/`DecomposeTRS`/`ComposeTRS`; `SampleAnimation`
  refactored to a wrapper; `test_blend` green (incl. refactor-equivalence).
- `AnimationComponent` extended (ClipB/TimeB/BlendWeight, additive, `value()`-default deser); round-trips.
- GameThread blends two clips into the palette (reusing SP2 transport); single-clip + bind-pose paths intact.
- CesiumMan + Fox committed + load at scale (mesh+skeleton+skin+texture); texture-path resolution fixed
  if needed.
- `AnimationEditor` Clip B + Blend Weight; Fox Walk↔Run blends on the slider (manual smoke).
- Full tree builds; all suites green; no `GAME_API_VERSION` bump.

## Notes

- The sampler refactor (pose-TRS + separate walk) is the structural enabler — SP5 (state machine) and
  any future N-way blend build on `BlendPoses`/`PoseToGlobals`. SP3's `SampleAnimation` stays as a
  single-clip convenience wrapper.
- Blending in **local TRS** then walking is the correct order; slerp needs quaternions, hence `BonePose`
  carries a quat rather than a matrix.
- Independent time cursors are a deliberate SP4 simplification; **phase-sync (gait matching) is SP5
  locomotion** — flagged so it's not forgotten.
- Textured humanoids exercise the material/texture load path for the first time; expect (and fix) a
  relative-texture-path resolution issue.
