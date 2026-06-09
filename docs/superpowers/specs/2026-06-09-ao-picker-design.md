# AO Algorithm Picker (Off / SSAO / HBAO / GTAO) Design

**Status:** Design approved, pre-implementation.
**Branch:** `feat/ao-picker`
**Depends on:** the SSAO compute pass (`SsaoRenderPass`, compute AO + LDS blur, shipped f987b31) and the AA-picker pattern (`AAMode` enum + `ResolveAaMode` int-based migration, `AaModeMigration.h` + `test_aamode`).

## Goal

Replace the SSAO on/off toggle with an **AO mode picker** — `Off / SSAO / HBAO / GTAO` — mirroring the AA `AAMode` pattern. HBAO and single-frame GTAO are added as compute-shader variants swapped into the existing `SsaoRenderPass` AO dispatch. Same pass I/O (G-buffer world normal `t1` + world pos `t2` + point sampler `s3` → R16F AO UAV `u4`), same `SsaoCB`, same LDS box blur. No lighting-shader change.

## Background / current state

- `SsaoRenderPass` (`src/engine/src/rendering/passes/SsaoRenderPass.{h,cpp}`): one AO compute shader `AO_CS` (hemisphere-kernel SSAO) + a `BLUR_CS` (8×8 group, 11×11 LDS tile, 4×4 box). `m_AoCS`/`m_BlurCS` shaders; `m_AoLayout` (b0 CB, t1 normal, t2 worldpos, s3 sampler, u4 out) + `m_BlurLayout` (b0, t1, u2); lazily-built `m_AoPipeline`/`m_BlurPipeline`. `Render` gates on `if (!GetSsaoSettings().Enabled) return;`, fills `SsaoCB{ ViewProj, View, Kernel[16], Params(x=Radius,y=Bias,z=Intensity,w=Power), RtSize }`, dispatches AO then blur.
- `SsaoSettings` (`RenderStats.h`): `{ bool Enabled; float Radius, Intensity, Power, Bias; }`.
- `LightingRenderPass`: multiplies the **ambient term only** by the blurred AO when `uSsaoEnabled != 0` (CB int); reads the AO texture at `t8`. Mode-agnostic — it does not care which algorithm produced the AO.
- Settings: `ApplicationSettings.ssaoEnabled` (bool) + `ssaoRadius/Intensity/Power/Bias`; persisted `renderer.ssao.{enabled,radius,intensity,power,bias}` (`SettingsManager` load/save); seeded into `GetSsaoSettings()` in `Application::Init`; persisted change-guarded in `ImGuiRenderer`; editor checkbox + sliders in `RenderStatsPanel`.
- AA reference: `AAMode { Off, FXAA, SMAA }`; `ResolveAaMode(renderer json)` (int `aaMode` precedence → legacy `fxaa` bool → default); `ApplicationSettings.aaMode` int; editor combo; `test_aamode`.

Per the early-dev breaking-changes policy, replacing the `ssao.enabled` bool with a mode int (with migration) is fine.

## Architecture

### Mode enum + pipeline selection
- New `enum class AoMode : int { Off = 0, SSAO = 1, HBAO = 2, GTAO = 3 };` in `RenderStats.h`. `SsaoSettings::Enabled` → `AoMode Mode = AoMode::SSAO;`. Keep `Radius/Intensity/Power/Bias`.
- `SsaoRenderPass` gains shader strings `HBAO_CS` + `GTAO_CS`, compiled at `Initialize` to `m_HbaoCS`/`m_GtaoCS`, lazily built into `m_HbaoPipeline`/`m_GtaoPipeline` reusing the **existing `m_AoLayout`** (identical bindings). `Render`: read `GetSsaoSettings().Mode`; `if (Mode == AoMode::Off) return;`; select the AO pipeline by mode (`SSAO→m_AoPipeline`, `HBAO→m_HbaoPipeline`, `GTAO→m_GtaoPipeline`); the blur dispatch is unchanged. Resize teardown drops the two new pipelines alongside the existing ones.
- `SsaoCB` is **unchanged**. HBAO/GTAO reuse `Params.x=Radius`, `Params.z=Intensity`, `Params.w=Power`; they ignore `Params.y=Bias` (SSAO-only) and `Kernel[16]` (SSAO-only). Directions (4) and steps (8) are compile-time `#define`s in the HBAO/GTAO shaders.

### HBAO_CS (horizon-based, single-frame, screen-space)
`[numthreads(8,8,1)]`, one thread per pixel, same bounds-guard + early-out (`dot(N,N) < 0.5 → uOut=1`) as `AO_CS`. Reconstruct view-space position/normal from the world-pos G-buffer via `uView` (matching SSAO's RH, negative view-Z convention). For each of `kDirections` (4) screen-space directions — rotated per pixel by the SAME 4×4 tile hash `AO_CS` uses (so the 4×4 blur cancels the rotation cleanly) — march `kSteps` (8) samples outward within world-space `Radius`, sampling `uWorldPos` (point sampler) at each, tracking the maximum horizon angle (sin) seen. AO = `1 − mean(sin(horizon))` over directions; `pow(saturate(1 − occ·Intensity), Power)`. Write `uOut[px]`.

### GTAO_CS (ground-truth visibility integral, single-frame)
Same horizon march, but per slice integrate the **cosine-weighted visibility arc** relative to the surface normal projected into the slice plane (the GTAO inner integral: clamp horizons to the normal hemisphere, integrate `cos`-weighted), averaged over slices. More physically correct than HBAO's plain horizon average. Single-frame → noisier; the existing 4×4 LDS box blur denoises it (no temporal accumulation — deferred). Reuses `Radius/Intensity/Power`. Writes the same `u4` UAV.

Both shaders bind EXACTLY `b0` CB / `t1` normal / `t2` worldpos / `s3` sampler / `u4` out — identical to `AO_CS` — so they share `m_AoLayout` (no flat-binding drift). Both feed the unchanged `BLUR_CS`.

### Settings / migration (mirror AA)
- New `src/common/include/AoModeMigration.h`: `int ResolveAoMode(const nlohmann::json& ssao)` — precedence: (1) `mode` int in `[0,3]` → use it; (2) legacy `enabled` bool → true⇒SSAO(1), false⇒Off(0); (3) missing/out-of-range ⇒ default SSAO(1). Int-based, dependency-free (no engine enum), unit-testable — same shape as `ResolveAaMode`.
- `ApplicationSettings`: `bool ssaoEnabled` → `int ssaoMode = 1`. Keep `ssaoRadius/Intensity/Power/Bias`.
- `SettingsManager`: **load** — replace the `enabled` read with `out->ssaoMode = ResolveAoMode(jr["ssao"])` (handles the legacy bool); keep radius/intensity/power/bias reads. **save** — write `renderer.ssao.mode = settings.ssaoMode` (drop `enabled`); keep the rest.
- `Application::Init`: seed `GetSsaoSettings().Mode = static_cast<AoMode>(m_AppContext->Settings.ssaoMode);` (replace the `Enabled` copy); keep the radius/intensity/power/bias copies.
- `ImGuiRenderer` shadow/ssao persist block: replace the `ssaoEnabled`↔`Enabled` dirty-check + writeback with `ssaoMode`↔`Mode` (cast). SSAO bias/radius/etc. unchanged.

### Lighting gate (no shader change)
CPU sets `cb.SsaoEnabled = (GetSsaoSettings().Mode != AoMode::Off) ? 1 : 0;` wherever it's currently set from `GetSsaoSettings().Enabled` (`LightingRenderPass` CB populate). The lighting HLSL is untouched.

### Editor
`RenderStatsPanel`: replace the "SSAO enabled" `Checkbox` with an `Off/SSAO/HBAO/GTAO` combo writing `sh.Mode` (mirror the existing AA `AAMode` combo in the same panel — same `changed |=` deferred-persist idiom). Keep the Radius/Intensity/Power sliders; relabel "SSAO bias" → "SSAO bias (SSAO only)". The AO sliders may be shown for any non-Off mode.

## Testing

- **Unit (new):** `tests/test_aomode.cpp` (mirror `test_aamode.cpp`) for `ResolveAoMode`: `mode` int in range wins; out-of-range `mode` falls through; legacy `enabled:true`→1, `enabled:false`→0; neither→1 (default SSAO). Add a `test_aomode` target (mirror `test_aamode`'s CMake block).
- **Existing:** `test_ssaomath` stays green (hemisphere kernel unchanged — SSAO mode still uses it). `test_aamode` untouched.
- **Smoke (manual GUI, `msvc-win64-vs2026-community`):** cycle the AO combo Off→SSAO→HBAO→GTAO live; AO darkens creases/contact in all three non-Off modes; HBAO/GTAO read crisper / less self-haloing than SSAO; toggle Off → AO gone, scene otherwise unchanged; restart → mode persists; an old `engine_settings.json` with `renderer.ssao.enabled` migrates (true→SSAO, false→Off); no NVRHI validation errors (two extra compute pipelines, same layout/CB/UAV).

## Scope

**In:** `AoMode` enum + `SsaoSettings::Mode`; `HBAO_CS` + single-frame `GTAO_CS` reusing `m_AoLayout`/`SsaoCB`/`BLUR_CS`; mode-selected AO pipeline; `ResolveAoMode` + `AoModeMigration.h` + `test_aomode`; settings load/save/seed/persist migration (`ssaoEnabled`→`ssaoMode`); editor combo; the `uSsaoEnabled = Mode != Off` gate.

**Out:** GTAO temporal accumulation / history / reprojection; bent normals; multi-bounce; per-mode direction/step sliders (hardcoded `#define`s); any change to `BLUR_CS` or the lighting shader; applying AO to the full composite (still ambient-only, parked look-knob).

## Risks / notes

- **GTAO single-frame noise:** the 4×4 box blur may leave residual noise on GTAO; acceptable for v1 (temporal is the named follow-up). `kDirections`/`kSteps` are the quality/cost dial.
- **View-space reconstruction parity:** HBAO/GTAO derive view P/N from the world-pos G-buffer via `uView`; must match SSAO's existing RH / negative-view-Z convention (the working `AO_CS` is the reference) or occlusion sign flips.
- **Binding parity:** HBAO/GTAO MUST bind `b0/t1/t2/s3/u4` exactly like `AO_CS` to reuse `m_AoLayout` (the recurring NVRHI flat-binding rule). A mismatch is a validation error / AV.
- **Default mode:** old SSAO default was `Enabled = true`; `ResolveAoMode` default + the new `Mode = AoMode::SSAO` default preserve "SSAO on" for existing users.
- **Resize/teardown:** the two new pipelines must be dropped + rebuilt on backend swap/resize exactly like `m_AoPipeline` (lazy rebuild in `Render`), or they dangle against a destroyed device.
- **CB unchanged:** no `SsaoCB` layout change → no alignment risk; HBAO/GTAO simply read a subset.
