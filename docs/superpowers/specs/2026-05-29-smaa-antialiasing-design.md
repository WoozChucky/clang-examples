# SMAA Anti-Aliasing (replace FXAA-or-off bool with AA mode enum)

**Date:** 2026-05-29
**Status:** Approved, pending implementation plan

## Problem

The renderer offers FXAA as its only anti-aliasing option, toggled by a single `bool`. FXAA
smooths edges but, being a luma-blur post-process, softens texture detail and text — a visible
quality cost the user wants to avoid. SMAA (pattern-based morphological AA) gives cleaner edges
*and* keeps textures sharp, while remaining a pure post-process that fits the deferred pipeline
(unlike MSAA) and needs no temporal infrastructure (unlike TAA).

## Goals

1. Add SMAA 1x as a selectable anti-aliasing technique.
2. Replace the `FxaaEnabled` bool with a three-way `AAMode { Off, FXAA, SMAA }`, migrating the
   persisted setting.
3. Keep FXAA working and the no-AA path intact.

## Non-Goals

- No SMAA S2x/T2x (those need MSAA / temporal — out of scope; deferred + no motion vectors).
- No runtime quality-preset UI (bake `SMAA_PRESET_HIGH`; changing presets is a one-line
  shader-macro edit + recompile).
- No TAA, no CAS sharpen, no motion vectors.
- No change to the deferred pass pipeline itself (SMAA is a Renderer-owned resolve, like FXAA).

## Background: current AA wiring (verified)

- `AntiAliasingSettings { bool FxaaEnabled = true; }` in `src/engine/src/rendering/RenderStats.h`
  — a RenderThread-only global accessed via `GetAntiAliasingSettings()` (exported from Engine.dll).
- `FxaaRenderPass` is **Renderer-owned** (`m_FxaaPass`), not part of `m_RenderPasses`.
- In `Renderer::Render`: when FXAA is on, the world passes render into an offscreen `SceneColor`
  SRV (`EnsureSceneColor`), then `m_FxaaPass->Render(...)` resolves it into the swapchain
  `sceneBuffer`; UI draws on top. When off, world passes draw straight to `sceneBuffer`. On
  scene-color alloc failure it falls back to direct render with a one-shot `SM_WARN`
  (`s_warnedFxaaAlloc`).
- Persistence (engine tier): `Settings.fxaaEnabled` (bool) in `ApplicationContext.h`, serialized
  as `j["renderer"]["fxaa"]` in `SettingsManager.cpp`. Startup sync at `Application.cpp:22`:
  `GetAntiAliasingSettings().FxaaEnabled = m_AppContext->Settings.fxaaEnabled;`. The editor's
  `ImGuiRenderer.cpp` (~line 331) writes the setting back to `engine_settings.json` only when it
  changed.
- Shaders are inline `R"()"` HLSL strings compiled via DXC through
  `Renderer::CreateShader(type, content, contentSize, entry, target)` — **no include handler**,
  so multi-file `#include` is not available; shader text must be assembled into one string.

## Design

### 1. Settings model + migration

In `src/engine/src/rendering/RenderStats.h`:
```cpp
enum class AAMode : int { Off = 0, FXAA = 1, SMAA = 2 };
struct AntiAliasingSettings { AAMode Mode = AAMode::FXAA; };
```

Persisted layer — in `ApplicationContext.h`, replace `bool fxaaEnabled = true;` with
`int aaMode = 1; // AAMode::FXAA` (stored as int to keep the common header free of the
Engine-side enum; the Engine maps int<->AAMode at the sync points).

`SettingsManager.cpp`:
- **Load (migration):** read `renderer.aaMode` (int) if present; else if legacy `renderer.fxaa`
  (bool) present, map `true`→1 (FXAA), `false`→0 (Off); else default 1 (FXAA).
- **Save:** write `renderer.aaMode` (int). Stop writing `renderer.fxaa`.

`Application.cpp:22` sync becomes:
`GetAntiAliasingSettings().Mode = static_cast<AAMode>(m_AppContext->Settings.aaMode);`

`ImGuiRenderer.cpp` (~line 331): compare/persist the int form of `GetAntiAliasingSettings().Mode`
against `m_AppContext->Settings.aaMode`, writing `engine_settings.json` only on change (same
change-guard pattern as today).

### 2. New pass: `SmaaRenderPass`

New `src/engine/src/rendering/passes/SmaaRenderPass.{h,cpp}` (listed in
`src/engine/CMakeLists.txt`), mirroring `FxaaRenderPass`'s structure (Initialize/Render/Shutdown/
OnResize, Renderer-owned `m_SmaaPass`). It occupies the same socket: input
`Renderer::GetSceneColorTexture()`, output the swapchain `sceneBuffer`.

SMAA 1x is three sub-passes, run in sequence inside `Render`:
1. **Edge detection** (`SMAALumaEdgeDetectionPS`): scene color → `edgesTex` (RG8), cleared to 0
   each frame.
2. **Blending-weight calc** (`SMAABlendingWeightCalculationPS`): `edgesTex` + `AreaTex` +
   `SearchTex` → `blendTex` (RGBA8), cleared to 0 each frame.
3. **Neighborhood blending** (`SMAANeighborhoodBlendingPS`): scene color + `blendTex` →
   `sceneBuffer`.

Each sub-pass is a full-screen triangle (SV_VertexID), its own pipeline + binding set, built
lazily and dropped on resize (matching FxaaRenderPass's lazy `m_Pipeline`). The two intermediate
RTs (`edgesTex`, `blendTex`) are full-resolution, owned by the pass, (re)created when the
framebuffer size changes (mirror `EnsureSceneColor`'s size-check pattern). SMAA also wants an
intermediate framebuffer per RT to render into.

### 3. Shader assembly (vendored official SMAA.hlsl)

- Vendor the official `SMAA.hlsl` (Jimenez et al., MIT) verbatim under `third_party/smaa/`.
- Because `CreateShader` has no include handler, embed the SMAA.hlsl text as a
  `static const char* SMAA_HLSL` (a generated string header, e.g. `third_party/smaa/SMAA_hlsl.h`)
  and **assemble each entry shader as one string**: `prelude (config macros) + SMAA_HLSL body +
  thin entry wrapper`. The official header is explicitly designed to be included after its
  configuration macros are defined.
- Shared prelude (single, clearly-commented config block — the preset is changed here):
```hlsl
#define SMAA_PRESET_HIGH 1     // <-- change to SMAA_PRESET_ULTRA / _LOW etc. + recompile
#define SMAA_HLSL_4 1
#define SMAA_RT_METRICS uRtMetrics   // float4(1/w, 1/h, w, h) from the CB
```
- The VS uses SMAA's `SMAA*VS` helpers (they emit the offset/pixcoord varyings the PS expects);
  the PS calls the corresponding `SMAA*PS`. Luma edge detection (cheap, standard, matches FXAA).

### 4. Lookup textures (embedded)

`AreaTex` (160×560, RG8) and `SearchTex` (64×16, R8) ship as the official byte arrays in a
vendored header (`third_party/smaa/SMAATextures.h`). Created once in `Initialize` via
`createTexture` + a staging `writeTexture`, held as SRVs, bound in sub-pass 2. No asset files,
no runtime loading. (Note the official SearchTex bytes are stored bottom-up; use the standard
upload as documented in the SMAA repo so V-orientation matches the shader.)

### 5. Renderer integration

Replace the `bool fxaa` decision in `Renderer::Render` with a switch on
`GetAntiAliasingSettings().Mode`:
- `Off` → existing direct-to-`sceneBuffer` path (no offscreen).
- `FXAA` → existing scene-color → `m_FxaaPass` path, unchanged.
- `SMAA` → scene-color path (reuse the same `EnsureSceneColor` offscreen target + the same
  alloc-failure fallback/one-shot `SM_WARN`), then `m_SmaaPass->Render(...)` instead of FXAA.

`Renderer` gains `m_SmaaPass` (init in `Init`/`InitForSwap`, `Shutdown`, `OnResize` forwarding),
exactly paralleling `m_FxaaPass`.

### 6. Constant buffer / bindings

Per-frame CB carries `SMAA_RT_METRICS = float4(1/w, 1/h, w, h)` (like FXAA's `RcpFrame`), written
from the framebuffer size. Bindings use the project's globally-unique b/t/s slot convention with
the Vulkan binding-offset block copied from `FxaaRenderPass`. Each sub-pass binds only what it
needs (e.g. edge: CB + scene + sampler; weights: CB + edges + AreaTex + SearchTex + samplers;
blend: CB + scene + blendTex + sampler). SMAA wants a linear-clamp sampler (and uses point reads
where required internally).

### 7. Error handling

- Any shader/texture/RT/framebuffer creation failure in `Initialize` → `SM_ERROR`, return false;
  `Renderer` logs the init failure and leaves `m_SmaaPass` null.
- If `Mode == SMAA` but `m_SmaaPass` is null or the offscreen scene-color target is unavailable →
  fall back to direct render (no AA) with a one-shot `SM_WARN`, mirroring the existing FXAA
  fallback. (Per project convention: log on degradation, never silent-skip.)
- `OnResize` → drop sub-pass pipelines + intermediate RTs/framebuffers so they rebuild at the new
  size.

### 8. Components & boundaries

| Unit | Responsibility | Depends on |
|------|----------------|------------|
| `AAMode` + `AntiAliasingSettings` | Runtime AA selection | none |
| `Settings.aaMode` + SettingsManager | Persist/migrate AA selection | nlohmann::json |
| `third_party/smaa/*` | Vendored SMAA.hlsl + lookup-texture bytes | none |
| `SmaaRenderPass` | 3-sub-pass SMAA resolve: scene color → swapchain | Renderer (scene color, CreateShader), nvrhi |
| `Renderer` AA switch | Route Off/FXAA/SMAA | both passes, EnsureSceneColor |
| `RenderStatsPanel` combo | Choose AA mode | AntiAliasingSettings |

### 9. Testing

- Render output isn't unit-testable (no offscreen GL/pass test harness in the repo). Correctness
  rests on: clean builds (`ecs`, `Engine`, `editor`) and manual visual verification.
- **Unit test (pure):** the settings migration mapping. Factor a free function
  `AAMode AAModeFromLegacyFxaa(bool)` (and document the load precedence) and test:
  legacy `true`→FXAA, `false`→Off; plus a JSON-load test (new `aaMode` key wins; legacy `fxaa`
  key maps when `aaMode` absent; neither → FXAA default). Add to a small test target (mirror the
  existing `test_atmosphere`/`test_editorprefs` style).
- **Manual verification:** cycle Off / FXAA / SMAA in the RenderStats panel; confirm SMAA edges
  are clean and textures sharper than FXAA; resize the window (no crash/garbage); restart and
  confirm the mode persisted; confirm an old `engine_settings.json` with `renderer.fxaa:true`
  loads as FXAA.

## Files Touched

- `src/engine/src/rendering/RenderStats.h` — `AAMode` enum, `AntiAliasingSettings.Mode`.
- `src/common/include/ApplicationContext.h` — `Settings.aaMode` (replaces `fxaaEnabled`).
- `src/engine/src/utilities/SettingsManager.cpp` — load-migration + save of `renderer.aaMode`.
- `src/engine/src/core/Application.cpp:22` — sync persisted int → `AntiAliasingSettings.Mode`.
- `third_party/smaa/SMAA.hlsl`, `SMAA_hlsl.h`, `SMAATextures.h` — vendored (MIT); engine include path.
- `src/engine/src/rendering/passes/SmaaRenderPass.{h,cpp}` — new (add to `src/engine/CMakeLists.txt`).
- `src/engine/src/rendering/Renderer.{h,cpp}` — own `m_SmaaPass`; AA-mode switch.
- `src/editor/src/panels/RenderStatsPanel.cpp` — FXAA checkbox → Off/FXAA/SMAA combo.
- `src/editor/src/app/ImGuiRenderer.cpp` — persist `aaMode` (change-guarded).
- A settings-migration unit test (+ `tests/CMakeLists.txt`).

## Licensing

SMAA is MIT-licensed (Jimenez et al.). Vendor its license header alongside `third_party/smaa/`.
