# SMAA Anti-Aliasing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add SMAA 1x as a selectable anti-aliasing technique, replacing the FXAA on/off bool with an `AAMode { Off, FXAA, SMAA }` enum (with persisted-setting migration).

**Architecture:** SMAA is a Renderer-owned post-process resolve occupying the same socket as the existing FXAA pass (offscreen scene-color SRV → swapchain). It runs three full-screen sub-passes (edge detection → blend-weight calc → neighborhood blend) using the vendored official `SMAA.hlsl` and its matched AreaTex/SearchTex lookup textures. The AA technique is chosen by an enum routed through `Renderer::Render`.

**Tech Stack:** C++23, NVRHI (DX12/Vulkan), DXC (HLSL SM6.1, inline shader strings — no `#include` handler), nlohmann::json, Dear ImGui, vendored SMAA (Jimenez et al., MIT).

**Spec:** `docs/superpowers/specs/2026-05-29-smaa-antialiasing-design.md`

---

## Build & Test Reference

- Configure: `cmake --preset msvc-win64-vs2026-community` (enterprise NOT installed — community only).
- Build a target: `cmake --build --preset msvc-win64-vs2026-community --target <ecs|Engine|editor|test_aamode>`
- Test exes land in `out/build/msvc-win64-vs2026-community/bin/Debug/`.
- Commit identity is configured globally; use `git commit` normally, never `--no-verify`.

## File Structure

| File | Responsibility | Task |
|------|----------------|------|
| `src/common/include/AaModeMigration.h` (new) | Pure `ResolveAaMode(json)` — migrate legacy `fxaa` bool → AA-mode int | 1 |
| `tests/test_aamode.cpp` (new) + `tests/CMakeLists.txt` | Unit-test the migration | 1 |
| `src/engine/src/rendering/RenderStats.h` | `AAMode` enum + `AntiAliasingSettings.Mode` | 2 |
| `src/common/include/ApplicationContext.h` | `ApplicationSettings.aaMode` (replaces `fxaaEnabled`) | 2 |
| `src/engine/src/utilities/SettingsManager.cpp` | Load (via `ResolveAaMode`) + save `renderer.aaMode` | 2 |
| `src/engine/src/core/Application.cpp` | Startup sync int → `AntiAliasingSettings.Mode` | 2 |
| `src/editor/src/app/ImGuiRenderer.cpp` | Persist AA mode (change-guarded) | 2 |
| `src/editor/src/panels/RenderStatsPanel.cpp` | FXAA checkbox → Off/FXAA/SMAA combo | 2 |
| `third_party/smaa/` (git submodule, already added) | Official SMAA.hlsl + `Textures/AreaTex.h`/`SearchTex.h` + LICENSE.txt | 3 |
| `<build>/generated/SMAA_hlsl.h` (CMake-generated, not committed) | SMAA.hlsl wrapped as a C string literal | 3 |
| `src/engine/src/rendering/passes/SmaaRenderPass.{h,cpp}` (new) | 3-sub-pass SMAA resolve | 4,5 |
| `src/engine/CMakeLists.txt` | Codegen SMAA_hlsl.h; add SmaaRenderPass.cpp + smaa include dirs | 3,4 |
| `src/engine/src/rendering/Renderer.{h,cpp}` | Own `m_SmaaPass`; AA-mode switch | 6 |

**Incremental milestones:** Task 1 = tested migration helper. Task 2 = enum fully working end-to-end with **FXAA/Off only** (no SMAA yet) — a shippable checkpoint. Tasks 3–5 build SMAA in isolation (compiles, unused). Task 6 wires SMAA in.

---

## Task 1: AA-mode migration helper (pure, TDD)

**Files:**
- Create: `src/common/include/AaModeMigration.h`
- Create: `tests/test_aamode.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_aamode.cpp`:

```cpp
#include <cstdio>
#include <nlohmann/json.hpp>

#include "AaModeMigration.h" // ResolveAaMode

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

// New aaMode key wins outright.
static void T00_aamode_key_wins()
{
    nlohmann::json r = { {"aaMode", 2}, {"fxaa", true} };
    EXPECT(ResolveAaMode(r) == 2); // SMAA, ignores legacy fxaa
}

// Legacy fxaa bool maps when aaMode absent: true -> FXAA(1), false -> Off(0).
static void T01_legacy_fxaa_maps()
{
    EXPECT(ResolveAaMode(nlohmann::json{ {"fxaa", true} })  == 1);
    EXPECT(ResolveAaMode(nlohmann::json{ {"fxaa", false} }) == 0);
}

// Neither key present -> default FXAA(1).
static void T02_default_is_fxaa()
{
    EXPECT(ResolveAaMode(nlohmann::json::object()) == 1);
}

// Out-of-range aaMode falls back (ignored -> legacy/default path).
static void T03_out_of_range_aamode_falls_back()
{
    EXPECT(ResolveAaMode(nlohmann::json{ {"aaMode", 7} })               == 1); // -> default
    EXPECT(ResolveAaMode(nlohmann::json{ {"aaMode", 7}, {"fxaa", false} }) == 0); // -> legacy
}

int main()
{
    T00_aamode_key_wins();
    T01_legacy_fxaa_maps();
    T02_default_is_fxaa();
    T03_out_of_range_aamode_falls_back();
    if (g_Failures == 0) { std::printf("All AA-mode tests passed.\n"); return 0; }
    std::fprintf(stderr, "%d AA-mode test(s) failed.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 2: Add the `test_aamode` target**

Append to `tests/CMakeLists.txt` (mirrors the existing `test_atmosphere` target's link to nlohmann; `RUNTIME_DIR` already defined):

```cmake
add_executable(test_aamode
    test_aamode.cpp
)

target_link_libraries(test_aamode PRIVATE
    nlohmann_json::nlohmann_json
)

target_include_directories(test_aamode PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
)

set_target_properties(test_aamode PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 3: Run test to verify it fails**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_aamode
```
Expected: FAIL to compile — `AaModeMigration.h` not found.

- [ ] **Step 4: Write the helper**

Create `src/common/include/AaModeMigration.h`:

```cpp
#pragma once

#include <nlohmann/json.hpp>

// Resolve the persisted anti-aliasing mode (0 = Off, 1 = FXAA, 2 = SMAA) from a settings
// "renderer" JSON object, migrating the legacy boolean "fxaa" key.
//
// Precedence:
//   1. "aaMode" (int, in range [0,2])         -> use it
//   2. legacy "fxaa" (bool)                    -> true => FXAA(1), false => Off(0)
//   3. neither / out-of-range aaMode           -> default FXAA(1)
//
// Kept dependency-free (int-based, no engine enum) so it is unit-testable and usable from
// the common settings layer; the Engine maps this int to/from its AAMode enum at the edges.
inline int ResolveAaMode(const nlohmann::json& renderer)
{
    if (renderer.contains("aaMode") && renderer["aaMode"].is_number_integer()) {
        const int m = renderer["aaMode"].get<int>();
        if (m >= 0 && m <= 2) return m;
    }
    if (renderer.contains("fxaa") && renderer["fxaa"].is_boolean()) {
        return renderer["fxaa"].get<bool>() ? 1 : 0;
    }
    return 1; // default FXAA
}
```

- [ ] **Step 5: Run test to verify it passes**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_aamode
./out/build/msvc-win64-vs2026-community/bin/Debug/test_aamode.exe
```
Expected: `All AA-mode tests passed.`

- [ ] **Step 6: Commit**

```bash
git add src/common/include/AaModeMigration.h tests/test_aamode.cpp tests/CMakeLists.txt
git commit -m "feat(settings): ResolveAaMode migration helper + test"
```

---

## Task 2: AAMode enum + settings wiring (FXAA/Off end-to-end, no SMAA yet)

**Files:**
- Modify: `src/engine/src/rendering/RenderStats.h:34-36`
- Modify: `src/common/include/ApplicationContext.h:17-23`
- Modify: `src/engine/src/utilities/SettingsManager.cpp:72-74` and `:94`
- Modify: `src/engine/src/core/Application.cpp:22`
- Modify: `src/editor/src/app/ImGuiRenderer.cpp` (~line 331 block)
- Modify: `src/editor/src/panels/RenderStatsPanel.cpp:47-48`

This task introduces the enum and makes Off/FXAA work through it. SMAA is a valid enum value but the renderer will treat "SMAA" as "no SMAA pass yet" until Task 6 — to keep this milestone shippable, the panel combo in this task lists only **Off / FXAA** (SMAA entry added in Task 6).

- [ ] **Step 1: Add the `AAMode` enum + `Mode` field**

In `src/engine/src/rendering/RenderStats.h`, replace:
```cpp
struct AntiAliasingSettings {
    bool FxaaEnabled = true;
};
```
with:
```cpp
enum class AAMode : int { Off = 0, FXAA = 1, SMAA = 2 };
struct AntiAliasingSettings {
    AAMode Mode = AAMode::FXAA;
};
```

- [ ] **Step 2: Replace the persisted field**

In `src/common/include/ApplicationContext.h`, in `struct ApplicationSettings`, replace:
```cpp
    bool        fxaaEnabled   = true;
```
with:
```cpp
    int         aaMode        = 1;   // 0 = Off, 1 = FXAA, 2 = SMAA (see AaModeMigration.h)
```

- [ ] **Step 3: Migrate load + save in SettingsManager**

In `src/engine/src/utilities/SettingsManager.cpp`, add the include near the top with the other includes:
```cpp
#include "AaModeMigration.h" // ResolveAaMode
```
Replace the load block (currently lines 72-74):
```cpp
        if (jr.contains("fxaa") && jr["fxaa"].is_boolean()) {
            out->fxaaEnabled = jr["fxaa"].get<bool>();
        }
```
with:
```cpp
        out->aaMode = ResolveAaMode(jr); // migrates legacy "fxaa" bool; defaults to FXAA
```
Replace the save line (currently line 94):
```cpp
    j["renderer"]["fxaa"]      = settings.fxaaEnabled;
```
with:
```cpp
    j["renderer"]["aaMode"]    = settings.aaMode;
```

- [ ] **Step 4: Fix the startup sync**

In `src/engine/src/core/Application.cpp`, line 22 currently:
```cpp
    GetAntiAliasingSettings().FxaaEnabled = m_AppContext->Settings.fxaaEnabled;
```
Replace with:
```cpp
    GetAntiAliasingSettings().Mode = static_cast<AAMode>(m_AppContext->Settings.aaMode);
```
(`AAMode` / `GetAntiAliasingSettings` come from `RenderStats.h`, already included by Application.cpp since it referenced `GetAntiAliasingSettings()` before. If the build reports `AAMode` undefined, add `#include "RenderStats.h"` — but verify it's missing before adding.)

- [ ] **Step 5: Update editor persistence**

In `src/editor/src/app/ImGuiRenderer.cpp`, the block around line 331 currently reads:
```cpp
            const bool fxaaNow = GetAntiAliasingSettings().FxaaEnabled;
            if (m_AppContext && fxaaNow != m_AppContext->Settings.fxaaEnabled) {
                m_AppContext->Settings.fxaaEnabled = fxaaNow;
                if (!SettingsManager::Save(SettingsManager::DEFAULT_SETTINGS_PATH, m_AppContext->Settings)) {
                    SM_WARN("Failed to persist FXAA toggle to %s", SettingsManager::DEFAULT_SETTINGS_PATH);
                }
            }
```
Replace with:
```cpp
            const int aaNow = static_cast<int>(GetAntiAliasingSettings().Mode);
            if (m_AppContext && aaNow != m_AppContext->Settings.aaMode) {
                m_AppContext->Settings.aaMode = aaNow;
                if (!SettingsManager::Save(SettingsManager::DEFAULT_SETTINGS_PATH, m_AppContext->Settings)) {
                    SM_WARN("Failed to persist AA mode to %s", SettingsManager::DEFAULT_SETTINGS_PATH);
                }
            }
```

- [ ] **Step 6: Update the panel (Off / FXAA combo for now)**

In `src/editor/src/panels/RenderStatsPanel.cpp`, the block at lines 47-48 currently:
```cpp
    AntiAliasingSettings& aa = GetAntiAliasingSettings();
    changed |= ImGui::Checkbox("FXAA", &aa.FxaaEnabled);
```
Replace with (SMAA option is added in Task 6 — keep the list to Off/FXAA here so this milestone never selects an unimplemented mode):
```cpp
    AntiAliasingSettings& aa = GetAntiAliasingSettings();
    {
        int mode = static_cast<int>(aa.Mode);
        if (mode > 1) mode = 1; // SMAA not wired yet this milestone -> clamp display to FXAA
        const char* names[] = { "Off", "FXAA" };
        if (ImGui::Combo("Anti-aliasing", &mode, names, IM_ARRAYSIZE(names))) {
            aa.Mode = static_cast<AAMode>(mode);
            changed = true;
        }
    }
```

- [ ] **Step 7: Build ecs + Engine + editor**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target ecs
cmake --build --preset msvc-win64-vs2026-community --target Engine
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: all clean. (Grep first to confirm no other reader of `FxaaEnabled`/`fxaaEnabled` remains: `grep -rn "FxaaEnabled\|fxaaEnabled" src` should return nothing. If it does, update those references the same way.)

- [ ] **Step 8: Run the migration + existing test sweep**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_aamode
./out/build/msvc-win64-vs2026-community/bin/Debug/test_aamode.exe
```
Expected: `All AA-mode tests passed.`

- [ ] **Step 9: Commit**

```bash
git add src/engine/src/rendering/RenderStats.h src/common/include/ApplicationContext.h src/engine/src/utilities/SettingsManager.cpp src/engine/src/core/Application.cpp src/editor/src/app/ImGuiRenderer.cpp src/editor/src/panels/RenderStatsPanel.cpp
git commit -m "feat(render): AA mode enum (Off/FXAA) replacing FxaaEnabled bool, with settings migration"
```

---

## Task 3: Wire the SMAA submodule + generate the embeddable shader header

**Files:**
- Modify: `src/engine/CMakeLists.txt` (codegen `SMAA_hlsl.h` from the submodule's `SMAA.hlsl`; add the `Textures` + generated include dirs)

The submodule is already present and checked out (verified):
- `third_party/smaa/SMAA.hlsl` (the reference shader, MIT)
- `third_party/smaa/Textures/AreaTex.h` — `areaTexBytes[]`, `AREATEX_WIDTH=160`, `AREATEX_HEIGHT=560`, `AREATEX_PITCH=(AREATEX_WIDTH*2)`
- `third_party/smaa/Textures/SearchTex.h` — `searchTexBytes[]`, `SEARCHTEX_WIDTH=64`, `SEARCHTEX_HEIGHT=16`, `SEARCHTEX_PITCH=SEARCHTEX_WIDTH`
- `third_party/smaa/LICENSE.txt`

**Do NOT edit any file under `third_party/smaa/`** — it is a pinned submodule. `Renderer::CreateShader` takes one source string and DXC has no `#include` handler, so we embed `SMAA.hlsl` as a C string literal via a **CMake configure-time codegen** (no manual paste, regenerates if the submodule updates).

- [ ] **Step 1: Add the codegen + include dirs to Engine**

In `src/engine/CMakeLists.txt`, before the `Engine` target's `target_include_directories(...)`, add a configure-time step that reads `SMAA.hlsl` and writes a string-literal header into the build tree. (Using `file(READ)`+`file(WRITE)` rather than `configure_file` — `SMAA.hlsl` contains `@token` sequences in comments that `configure_file`'s `@VAR@` substitution could mangle.)

```cmake
# --- Embed SMAA.hlsl as a C string literal (DXC has no #include handler) ---
set(_smaa_src "${CMAKE_SOURCE_DIR}/third_party/smaa/SMAA.hlsl")
set(_smaa_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_smaa_gen "${_smaa_gen_dir}/SMAA_hlsl.h")
file(MAKE_DIRECTORY "${_smaa_gen_dir}")
file(READ "${_smaa_src}" _smaa_body)
# R"SMAA( ... )SMAA" delimiter: the terminator )SMAA" does not occur in SMAA.hlsl.
file(WRITE  "${_smaa_gen}" "#pragma once\n// Generated from third_party/smaa/SMAA.hlsl (MIT). Do not edit.\nstatic const char* SMAA_HLSL_SOURCE = R\"SMAA(\n")
file(APPEND "${_smaa_gen}" "${_smaa_body}")
file(APPEND "${_smaa_gen}" "\n)SMAA\";\n")
```

Then add these to the `Engine` target's `target_include_directories(...)`:
```cmake
    ${CMAKE_SOURCE_DIR}/third_party/smaa/Textures
    ${CMAKE_CURRENT_BINARY_DIR}/generated
```

- [ ] **Step 2: Sanity-check the codegen guard**

Confirm the chosen raw-string terminator does not appear in the source (must print nothing):
```
grep -n ')SMAA"' third_party/smaa/SMAA.hlsl
```
Expected: no output. (If it ever matches, change the delimiter token in the codegen, e.g. `R"SMAA_RAW( ... )SMAA_RAW"`.)

- [ ] **Step 3: Configure + confirm the header generates and Engine still builds**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target Engine
```
Expected: clean. Confirm `out/build/msvc-win64-vs2026-community/src/engine/generated/SMAA_hlsl.h` exists and begins with the `SMAA_HLSL_SOURCE = R"SMAA(` line. (Nothing references it yet; this just validates codegen + include paths.)

- [ ] **Step 4: Commit**

```bash
git add src/engine/CMakeLists.txt
git commit -m "build(render): embed submodule SMAA.hlsl as string + wire smaa include dirs"
```

(The submodule registration itself — `.gitmodules` + the `third_party/smaa` gitlink — was added by the user; if `git status` shows it uncommitted, include it in this commit.)

---

## Task 4: SmaaRenderPass — Initialize (shaders, lookup textures, RTs)

**Files:**
- Create: `src/engine/src/rendering/passes/SmaaRenderPass.h`
- Create: `src/engine/src/rendering/passes/SmaaRenderPass.cpp`
- Modify: `src/engine/CMakeLists.txt` (add `SmaaRenderPass.cpp` to the Engine sources, next to `FxaaRenderPass.cpp`)

This task builds the pass's resources but does NOT wire it into the Renderer (Task 6). Read `src/engine/src/rendering/passes/FxaaRenderPass.{h,cpp}` first — this pass mirrors its structure, lifecycle, binding-layout convention, and Vulkan binding-offset handling.

- [ ] **Step 1: Write the header**

Create `src/engine/src/rendering/passes/SmaaRenderPass.h`:
```cpp
#pragma once

#include <nvrhi/nvrhi.h>
#include <vector>
#include <utility>

#include "IRenderPass.h"

class Renderer;

// SMAA 1x resolve (Renderer-owned, NOT in m_RenderPasses). Occupies the same socket as
// FxaaRenderPass: reads the offscreen scene-color SRV, writes the swapchain framebuffer.
// Three full-screen sub-passes: luma edge detection -> blend-weight calc -> neighborhood blend.
class SmaaRenderPass : public IRenderPass {
public:
    bool Initialize(nvrhi::IDevice* device, Renderer* renderer);
    void Render(nvrhi::ICommandList* commandList,
                nvrhi::IFramebuffer* frameBuffer,
                SimulationSnapshot& snapshot,
                const ECS* world,
                double deltaTime,
                FrameAllocator* frameAllocator) override;
    void Shutdown();
    void OnResize(uint32_t width, uint32_t height) override;

private:
    // (Re)create edges/blend intermediate targets + their framebuffers at the given size.
    bool EnsureTargets(uint32_t width, uint32_t height);

    nvrhi::IDevice* m_Device   = nullptr;
    Renderer*       m_Renderer = nullptr;

    // Shaders: shared full-screen VS per stage + per-stage PS.
    nvrhi::ShaderHandle m_EdgeVS, m_EdgePS;
    nvrhi::ShaderHandle m_WeightVS, m_WeightPS;
    nvrhi::ShaderHandle m_BlendVS, m_BlendPS;

    // Pipelines (lazy, rebuilt on resize like FxaaRenderPass).
    nvrhi::GraphicsPipelineHandle m_EdgePipeline, m_WeightPipeline, m_BlendPipeline;

    nvrhi::BindingLayoutHandle m_EdgeLayout, m_WeightLayout, m_BlendLayout;
    nvrhi::BufferHandle        m_FrameCB;     // SMAA_RT_METRICS
    nvrhi::SamplerHandle       m_LinearClamp; // linear clamp
    nvrhi::SamplerHandle       m_PointClamp;  // point clamp (SMAA uses point fetches internally)

    nvrhi::TextureHandle m_AreaTex;   // 160x560 RG8
    nvrhi::TextureHandle m_SearchTex; // 64x16 R8

    // Intermediate targets.
    uint32_t              m_Width = 0, m_Height = 0;
    nvrhi::TextureHandle  m_EdgesTex;   // RG8
    nvrhi::TextureHandle  m_BlendTex;   // RGBA8
    nvrhi::FramebufferHandle m_EdgesFb, m_BlendFb;
};
```

- [ ] **Step 2: Write Initialize (shaders + samplers + lookup textures + CB)**

Create `src/engine/src/rendering/passes/SmaaRenderPass.cpp`. Start with the shader assembly + Initialize. The shader sources concatenate a config prelude + the embedded `SMAA_HLSL_SOURCE` + a thin entry wrapper that adapts SMAA's `*VS` helpers to a SV_VertexID full-screen triangle.

```cpp
#include "SmaaRenderPass.h"

#include "Renderer.h"
#include <nvrhi/utils.h>
#include <string>

#include "SMAA_hlsl.h"   // SMAA_HLSL_SOURCE (generated from submodule SMAA.hlsl, MIT)
#include "AreaTex.h"     // areaTexBytes, AREATEX_WIDTH/HEIGHT/PITCH
#include "SearchTex.h"   // searchTexBytes, SEARCHTEX_WIDTH/HEIGHT/PITCH

// SMAA constant-buffer layout. SMAA_RT_METRICS = (1/w, 1/h, w, h).
struct SmaaFrameCB { glm::vec4 RtMetrics; };

namespace {
// Config prelude defined BEFORE the SMAA.hlsl body.
//
// We use SMAA_CUSTOM_SL (not SMAA_HLSL_4): SMAA.hlsl's built-in HLSL4 path declares its
// samplers with legacy Effects-framework state syntax (`SamplerState X { Filter=...; }`),
// which DXC/SM6 rejects. SMAA_CUSTOM_SL lets us supply the device macros + register-based
// samplers ourselves. Mirror of SMAA.hlsl's HLSL4 macro block (lines ~543-553) but with
// `register(s#)` samplers. The preset is changed on the first line.
static const char* SMAA_PRELUDE = R"(
#define SMAA_PRESET_HIGH 1          // <-- change to SMAA_PRESET_ULTRA / _LOW etc. + recompile
#define SMAA_RT_METRICS uRtMetrics
#define SMAA_CUSTOM_SL 1
cbuffer SmaaCB : register(b0) { float4 uRtMetrics; };
SamplerState LinearSampler : register(s0);
SamplerState PointSampler  : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
)";

// Full-screen-triangle VS that also produces the SMAA varyings each stage needs.
// SMAA's *VS helpers take a texcoord and fill offset[]/pixcoord; we synthesize texcoord
// from SV_VertexID instead of a vertex buffer. Textures live at t0.. ; the SMAA PS take the
// texture by name (the SMAATexture2D macro = `Texture2D tex`) and use the global samplers.
static const char* EDGE_VS = R"(
struct VSOut { float4 PosH:SV_POSITION; float2 tc:TEXCOORD0; float4 off[3]:TEXCOORD1; };
VSOut main_vs(uint vid : SV_VertexID){
    VSOut o; o.tc = float2((vid<<1)&2, vid&2);
    o.PosH = float4(o.tc*float2(2,-2)+float2(-1,1),0,1);
    SMAAEdgeDetectionVS(o.tc, o.off);
    return o;
}
)";
static const char* EDGE_PS = R"(
Texture2D uColor : register(t0);
struct PSIn { float4 PosH:SV_POSITION; float2 tc:TEXCOORD0; float4 off[3]:TEXCOORD1; };
float4 main_ps(PSIn i):SV_Target { return float4(SMAALumaEdgeDetectionPS(i.tc, i.off, uColor), 0.0, 0.0); }
)";

static const char* WEIGHT_VS = R"(
struct VSOut { float4 PosH:SV_POSITION; float2 tc:TEXCOORD0; float2 pix:TEXCOORD1; float4 off[3]:TEXCOORD2; };
VSOut main_vs(uint vid : SV_VertexID){
    VSOut o; o.tc = float2((vid<<1)&2, vid&2);
    o.PosH = float4(o.tc*float2(2,-2)+float2(-1,1),0,1);
    SMAABlendingWeightCalculationVS(o.tc, o.pix, o.off);
    return o;
}
)";
static const char* WEIGHT_PS = R"(
Texture2D uEdges : register(t0); Texture2D uArea : register(t1); Texture2D uSearch : register(t2);
struct PSIn { float4 PosH:SV_POSITION; float2 tc:TEXCOORD0; float2 pix:TEXCOORD1; float4 off[3]:TEXCOORD2; };
float4 main_ps(PSIn i):SV_Target {
    return SMAABlendingWeightCalculationPS(i.tc, i.pix, i.off, uEdges, uArea, uSearch, float4(0,0,0,0));
}
)";

static const char* BLEND_VS = R"(
struct VSOut { float4 PosH:SV_POSITION; float2 tc:TEXCOORD0; float4 off:TEXCOORD1; };
VSOut main_vs(uint vid : SV_VertexID){
    VSOut o; o.tc = float2((vid<<1)&2, vid&2);
    o.PosH = float4(o.tc*float2(2,-2)+float2(-1,1),0,1);
    SMAANeighborhoodBlendingVS(o.tc, o.off);
    return o;
}
)";
static const char* BLEND_PS = R"(
Texture2D uColor : register(t0); Texture2D uBlend : register(t1);
struct PSIn { float4 PosH:SV_POSITION; float2 tc:TEXCOORD0; float4 off:TEXCOORD1; };
float4 main_ps(PSIn i):SV_Target { return SMAANeighborhoodBlendingPS(i.tc, i.off, uColor, uBlend); }
)";

// Build a full shader source: prelude + SMAA body + entry wrapper.
static std::string Compose(const char* entry) {
    std::string s; s.reserve(96 * 1024);
    s += SMAA_PRELUDE; s += SMAA_HLSL_SOURCE; s += entry; return s;
}
} // namespace

bool SmaaRenderPass::Initialize(nvrhi::IDevice* device, Renderer* renderer)
{
    m_Device = device; m_Renderer = renderer;
    if (!m_Device || !m_Renderer) return false;

    auto mk = [&](nvrhi::ShaderType t, const char* stage, const char* entry, const char* prof) {
        const std::string src = Compose(stage);
        return m_Renderer->CreateShader(t, src.c_str(), src.size(), entry, prof);
    };
    m_EdgeVS   = mk(nvrhi::ShaderType::Vertex, EDGE_VS,   "main_vs", "vs_6_1");
    m_EdgePS   = mk(nvrhi::ShaderType::Pixel,  EDGE_PS,   "main_ps", "ps_6_1");
    m_WeightVS = mk(nvrhi::ShaderType::Vertex, WEIGHT_VS, "main_vs", "vs_6_1");
    m_WeightPS = mk(nvrhi::ShaderType::Pixel,  WEIGHT_PS, "main_ps", "ps_6_1");
    m_BlendVS  = mk(nvrhi::ShaderType::Vertex, BLEND_VS,  "main_vs", "vs_6_1");
    m_BlendPS  = mk(nvrhi::ShaderType::Pixel,  BLEND_PS,  "main_ps", "ps_6_1");
    if (!m_EdgeVS || !m_EdgePS || !m_WeightVS || !m_WeightPS || !m_BlendVS || !m_BlendPS) {
        SM_ERROR("SmaaRenderPass: shader compilation failed");
        return false;
    }

    { nvrhi::SamplerDesc sd; sd.setAllFilters(true);  sd.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp); m_LinearClamp = m_Device->createSampler(sd); }
    { nvrhi::SamplerDesc sd; sd.setAllFilters(false); sd.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp); m_PointClamp  = m_Device->createSampler(sd); }

    // Lookup textures from the vendored byte arrays.
    auto upload = [&](const unsigned char* bytes, uint32_t w, uint32_t h, nvrhi::Format fmt, uint32_t rowPitch, const char* name) -> nvrhi::TextureHandle {
        nvrhi::TextureDesc td; td.width = w; td.height = h; td.format = fmt;
        td.dimension = nvrhi::TextureDimension::Texture2D; td.debugName = name;
        td.initialState = nvrhi::ResourceStates::ShaderResource; td.keepInitialState = true;
        nvrhi::TextureHandle tex = m_Device->createTexture(td);
        // One-shot upload on a temp command list.
        nvrhi::CommandListHandle cl = m_Device->createCommandList();
        cl->open();
        cl->writeTexture(tex, /*arraySlice*/0, /*mipLevel*/0, bytes, rowPitch);
        cl->close();
        m_Device->executeCommandList(cl);
        return tex;
    };
    m_AreaTex   = upload(areaTexBytes,   AREATEX_WIDTH,   AREATEX_HEIGHT,   nvrhi::Format::RG8_UNORM, AREATEX_PITCH,   "SMAA AreaTex");
    m_SearchTex = upload(searchTexBytes, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, nvrhi::Format::R8_UNORM,  SEARCHTEX_PITCH, "SMAA SearchTex");
    if (!m_AreaTex || !m_SearchTex) { SM_ERROR("SmaaRenderPass: lookup texture creation failed"); return false; }

    // Binding layouts (unique b/t/s slots; Vulkan offsets like FxaaRenderPass).
    auto makeLayout = [&](std::vector<nvrhi::BindingLayoutItem> items) {
        nvrhi::BindingLayoutDesc d; d.visibility = nvrhi::ShaderType::All; d.bindings = std::move(items);
        if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
            d.setBindingOffsets(nvrhi::VulkanBindingOffsets{}.setConstantBufferOffset(0).setShaderResourceOffset(0).setSamplerOffset(0));
        return m_Device->createBindingLayout(d);
    };
    // Samplers are fixed globals (s0 Linear, s1 Point) from the SMAA_CUSTOM_SL prelude; textures at t0..
    m_EdgeLayout   = makeLayout({ nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(0), nvrhi::BindingLayoutItem::Sampler(1) });
    m_WeightLayout = makeLayout({ nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Texture_SRV(2), nvrhi::BindingLayoutItem::Sampler(0), nvrhi::BindingLayoutItem::Sampler(1) });
    m_BlendLayout  = makeLayout({ nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Sampler(0), nvrhi::BindingLayoutItem::Sampler(1) });
    if (!m_EdgeLayout || !m_WeightLayout || !m_BlendLayout) { SM_ERROR("SmaaRenderPass: binding layout creation failed"); return false; }

    m_FrameCB = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(SmaaFrameCB), "SmaaRenderPass FrameCB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer).setKeepInitialState(true));
    return m_FrameCB != nullptr;
}
```

Note on `AREATEX_PITCH`/`SEARCHTEX_PITCH`: the official headers define `*_PITCH` macros (bytes per row). If a given header version omits them, compute inline: AreaTex pitch = `AREATEX_WIDTH * 2` (RG), SearchTex pitch = `SEARCHTEX_WIDTH * 1` (R). Use whichever the vendored headers provide; verify by opening `third_party/smaa/AreaTex.h`.

- [ ] **Step 3: Add Shutdown + OnResize + EnsureTargets stubs (compile-only for now)**

Append to `SmaaRenderPass.cpp`:
```cpp
bool SmaaRenderPass::EnsureTargets(uint32_t width, uint32_t height)
{
    if (m_EdgesTex && m_Width == width && m_Height == height) return true;
    m_Width = width; m_Height = height;

    auto rt = [&](nvrhi::Format fmt, const char* name) {
        nvrhi::TextureDesc td; td.width = width; td.height = height; td.format = fmt;
        td.dimension = nvrhi::TextureDimension::Texture2D; td.isRenderTarget = true;
        td.debugName = name; td.initialState = nvrhi::ResourceStates::ShaderResource; td.keepInitialState = true;
        td.clearValue = nvrhi::Color(0.f); td.useClearValue = true;
        return m_Device->createTexture(td);
    };
    m_EdgesTex = rt(nvrhi::Format::RG8_UNORM, "SMAA edges");
    m_BlendTex = rt(nvrhi::Format::RGBA8_UNORM, "SMAA blend");
    if (!m_EdgesTex || !m_BlendTex) { SM_ERROR("SmaaRenderPass: intermediate target alloc failed"); return false; }
    m_EdgesFb = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_EdgesTex));
    m_BlendFb = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_BlendTex));
    return m_EdgesFb && m_BlendFb;
}

void SmaaRenderPass::Shutdown()
{
    m_EdgePipeline = m_WeightPipeline = m_BlendPipeline = nullptr;
    m_EdgeLayout = m_WeightLayout = m_BlendLayout = nullptr;
    m_EdgesFb = m_BlendFb = nullptr; m_EdgesTex = m_BlendTex = nullptr;
    m_AreaTex = m_SearchTex = nullptr; m_FrameCB = nullptr;
    m_LinearClamp = m_PointClamp = nullptr;
    m_EdgeVS = m_EdgePS = m_WeightVS = m_WeightPS = m_BlendVS = m_BlendPS = nullptr;
    m_Device = nullptr; m_Renderer = nullptr; m_Width = m_Height = 0;
}

void SmaaRenderPass::OnResize(uint32_t /*width*/, uint32_t /*height*/)
{
    m_EdgePipeline = m_WeightPipeline = m_BlendPipeline = nullptr;
    m_EdgesFb = m_BlendFb = nullptr; m_EdgesTex = m_BlendTex = nullptr;
    m_Width = m_Height = 0; // force EnsureTargets to rebuild
}

void SmaaRenderPass::Render(nvrhi::ICommandList*, nvrhi::IFramebuffer*, SimulationSnapshot&, const ECS*, double, FrameAllocator*)
{
    // Implemented in Task 5.
}
```

- [ ] **Step 4: Add the source to the Engine target**

In `src/engine/CMakeLists.txt`, find `src/rendering/passes/FxaaRenderPass.cpp` in the Engine sources and add right after it:
```cmake
    src/rendering/passes/SmaaRenderPass.cpp
```

- [ ] **Step 5: Build Engine**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target Engine
```
Expected: clean. This is where the embedded `SMAA_HLSL_SOURCE` string and the SMAA functions
(`SMAAEdgeDetectionVS`, `SMAALumaEdgeDetectionPS`, etc.) are first compiled by DXC, using the
`SMAA_CUSTOM_SL` device macros from the prelude. The prelude must appear **before**
`SMAA_HLSL_SOURCE` in the composed string (it does — `Compose` concatenates prelude + body +
entry). If DXC errors on a missing macro, cross-check the prelude against SMAA.hlsl's HLSL4
macro block (lines ~543-553) and add the missing one with `register`-based samplers.

- [ ] **Step 6: Commit**

```bash
git add src/engine/src/rendering/passes/SmaaRenderPass.h src/engine/src/rendering/passes/SmaaRenderPass.cpp src/engine/CMakeLists.txt
git commit -m "feat(render): SmaaRenderPass resources (shaders, lookup textures, targets)"
```

---

## Task 5: SmaaRenderPass — Render (the three sub-passes)

**Files:**
- Modify: `src/engine/src/rendering/passes/SmaaRenderPass.cpp` (replace the `Render` stub)

- [ ] **Step 1: Implement Render**

Replace the `Render` stub in `SmaaRenderPass.cpp` with:
```cpp
void SmaaRenderPass::Render(nvrhi::ICommandList* commandList,
                            nvrhi::IFramebuffer* frameBuffer,
                            SimulationSnapshot& /*snapshot*/,
                            const ECS* /*world*/,
                            double /*deltaTime*/,
                            FrameAllocator* /*frameAllocator*/)
{
    nvrhi::ITexture* scene = m_Renderer->GetSceneColorTexture();
    if (!scene) return;

    const auto fbi = frameBuffer->getFramebufferInfo();
    if (!EnsureTargets(fbi.width, fbi.height)) return;

    // Lazy pipelines (rebuilt on resize). Each sub-pass: no depth, no cull, no blend.
    auto makePipe = [&](nvrhi::IShader* vs, nvrhi::IShader* ps, nvrhi::IBindingLayout* layout, nvrhi::IFramebuffer* fb) {
        nvrhi::GraphicsPipelineDesc pso;
        pso.VS = vs; pso.PS = ps; pso.bindingLayouts = { layout };
        pso.primType = nvrhi::PrimitiveType::TriangleList;
        pso.renderState.depthStencilState.depthTestEnable = false;
        pso.renderState.depthStencilState.depthWriteEnable = false;
        pso.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        nvrhi::BlendState::RenderTarget rt; rt.setBlendEnable(false).setColorWriteMask(nvrhi::ColorMask::All);
        pso.renderState.blendState.setRenderTarget(0, rt);
        return m_Device->createGraphicsPipeline(pso, fb);
    };
    if (!m_EdgePipeline)   m_EdgePipeline   = makePipe(m_EdgeVS,   m_EdgePS,   m_EdgeLayout,   m_EdgesFb);
    if (!m_WeightPipeline) m_WeightPipeline = makePipe(m_WeightVS, m_WeightPS, m_WeightLayout, m_BlendFb);
    if (!m_BlendPipeline)  m_BlendPipeline  = makePipe(m_BlendVS,  m_BlendPS,  m_BlendLayout,  frameBuffer);

    commandList->beginMarker("SmaaRenderPass");

    SmaaFrameCB cb{};
    cb.RtMetrics = glm::vec4(1.0f / fbi.width, 1.0f / fbi.height, (float)fbi.width, (float)fbi.height);
    commandList->writeBuffer(m_FrameCB, &cb, sizeof(cb));

    auto draw = [&](nvrhi::IGraphicsPipeline* pipe, nvrhi::IFramebuffer* fb, nvrhi::BindingSetHandle bs) {
        nvrhi::GraphicsState st; st.pipeline = pipe; st.framebuffer = fb; st.bindings = { bs };
        st.viewport.addViewportAndScissorRect(fb->getFramebufferInfo().getViewport());
        commandList->setGraphicsState(st);
        nvrhi::DrawArguments a; a.vertexCount = 3; commandList->draw(a);
    };

    // 1) Edge detection: scene -> edges. Clear edges first (SMAA expects fresh edges).
    commandList->clearTextureFloat(m_EdgesTex, nvrhi::AllSubresources, nvrhi::Color(0.f));
    {
        nvrhi::BindingSetDesc d; d.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameCB),
            nvrhi::BindingSetItem::Texture_SRV(0, scene),
            nvrhi::BindingSetItem::Sampler(0, m_LinearClamp),
            nvrhi::BindingSetItem::Sampler(1, m_PointClamp) };
        draw(m_EdgePipeline, m_EdgesFb, m_Device->createBindingSet(d, m_EdgeLayout));
    }
    // 2) Blend-weight calc: edges + area + search -> blend. Clear blend first.
    commandList->clearTextureFloat(m_BlendTex, nvrhi::AllSubresources, nvrhi::Color(0.f));
    {
        nvrhi::BindingSetDesc d; d.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameCB),
            nvrhi::BindingSetItem::Texture_SRV(0, m_EdgesTex),
            nvrhi::BindingSetItem::Texture_SRV(1, m_AreaTex),
            nvrhi::BindingSetItem::Texture_SRV(2, m_SearchTex),
            nvrhi::BindingSetItem::Sampler(0, m_LinearClamp),
            nvrhi::BindingSetItem::Sampler(1, m_PointClamp) };
        draw(m_WeightPipeline, m_BlendFb, m_Device->createBindingSet(d, m_WeightLayout));
    }
    // 3) Neighborhood blend: scene + blend -> swapchain.
    {
        nvrhi::BindingSetDesc d; d.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameCB),
            nvrhi::BindingSetItem::Texture_SRV(0, scene),
            nvrhi::BindingSetItem::Texture_SRV(1, m_BlendTex),
            nvrhi::BindingSetItem::Sampler(0, m_LinearClamp),
            nvrhi::BindingSetItem::Sampler(1, m_PointClamp) };
        draw(m_BlendPipeline, frameBuffer, m_Device->createBindingSet(d, m_BlendLayout));
    }

    commandList->endMarker();
}
```

- [ ] **Step 2: Build Engine**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target Engine
```
Expected: clean. (If `clearTextureFloat`'s signature differs in this NVRHI version, match it to the one already used elsewhere in the renderer — grep `clearTextureFloat` under `src/engine/src/rendering`. If render-target textures need an explicit clear via framebuffer instead, use the same clear approach the other passes use for their targets.)

- [ ] **Step 3: Commit**

```bash
git add src/engine/src/rendering/passes/SmaaRenderPass.cpp
git commit -m "feat(render): SmaaRenderPass 3-sub-pass resolve (edges -> weights -> blend)"
```

---

## Task 6: Wire SMAA into the Renderer + panel

**Files:**
- Modify: `src/engine/src/rendering/Renderer.h` (add `#include "passes/SmaaRenderPass.h"` + `m_SmaaPass` member)
- Modify: `src/engine/src/rendering/Renderer.cpp` (init/shutdown/resize + AA-mode switch)
- Modify: `src/editor/src/panels/RenderStatsPanel.cpp` (add SMAA to the combo)

Read the FXAA-related regions of `Renderer.cpp` first: init (~line 144-148), shutdown (~line 177), the AA decision + world-target + resolve block (~line 265-322), and OnResize (~line 374).

- [ ] **Step 1: Declare the pass**

In `src/engine/src/rendering/Renderer.h`, next to `#include "passes/FxaaRenderPass.h"` (line 21) add:
```cpp
#include "passes/SmaaRenderPass.h"
```
Next to the `m_FxaaPass` member declaration add:
```cpp
    std::unique_ptr<SmaaRenderPass> m_SmaaPass;
```

- [ ] **Step 2: Initialize / shutdown / resize**

In `Renderer.cpp`, right after the `m_FxaaPass` init block (~line 144-148) add:
```cpp
    m_SmaaPass = std::make_unique<SmaaRenderPass>();
    if (!m_SmaaPass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize SmaaRenderPass");
        m_SmaaPass.reset(); // SMAA unavailable; AA switch will fall back with a one-shot warn
    }
```
After the `m_FxaaPass` shutdown line (~line 177) add:
```cpp
    if (m_SmaaPass) { m_SmaaPass->Shutdown(); m_SmaaPass.reset(); }
```
After the `m_FxaaPass->OnResize(...)` line (~line 374) add:
```cpp
    if (m_SmaaPass) { m_SmaaPass->OnResize(width, height); }
```

- [ ] **Step 3: Route the AA mode**

In `Renderer.cpp`, the current FXAA decision (~line 268) reads:
```cpp
                bool fxaa = GetAntiAliasingSettings().FxaaEnabled && m_FxaaPass != nullptr;
```
This already changed in Task 2 (no more `FxaaEnabled`). Replace the whole AA decision so it derives an "AA technique" from the enum. Use this structure (adapt the surrounding offscreen-target plumbing, which is shared by both AA techniques):
```cpp
                const AAMode aaMode = GetAntiAliasingSettings().Mode;
                bool useFxaa = (aaMode == AAMode::FXAA) && m_FxaaPass != nullptr;
                bool useSmaa = (aaMode == AAMode::SMAA) && m_SmaaPass != nullptr;
                if (aaMode == AAMode::SMAA && m_SmaaPass == nullptr) {
                    static bool s_warnedSmaa = false;
                    if (!s_warnedSmaa) { SM_WARN("SMAA selected but pass unavailable; rendering without AA"); s_warnedSmaa = true; }
                }
                bool offscreen = useFxaa || useSmaa; // both resolve from an offscreen scene-color SRV
```
Then everywhere the old code used `fxaa` to decide on the offscreen target, use `offscreen` (the `EnsureSceneColor` + `worldTarget = m_SceneColor.Fb` block and its alloc-failure fallback — on failure set `offscreen = useFxaa = useSmaa = false`).
Finally, replace the resolve block (~line 321):
```cpp
                if (fxaa) {
                    m_FxaaPass->Render(m_CommandList, sceneBuffer, snapshot, world, deltaTime, &m_FrameAllocator);
                }
```
with:
```cpp
                if (useFxaa) {
                    m_FxaaPass->Render(m_CommandList, sceneBuffer, snapshot, world, deltaTime, &m_FrameAllocator);
                } else if (useSmaa) {
                    m_SmaaPass->Render(m_CommandList, sceneBuffer, snapshot, world, deltaTime, &m_FrameAllocator);
                }
```

- [ ] **Step 4: Add SMAA to the panel combo**

In `src/editor/src/panels/RenderStatsPanel.cpp`, replace the Task-2 Off/FXAA combo block with the full three-way:
```cpp
    AntiAliasingSettings& aa = GetAntiAliasingSettings();
    {
        int mode = static_cast<int>(aa.Mode);
        const char* names[] = { "Off", "FXAA", "SMAA" };
        if (ImGui::Combo("Anti-aliasing", &mode, names, IM_ARRAYSIZE(names))) {
            aa.Mode = static_cast<AAMode>(mode);
            changed = true;
        }
    }
```

- [ ] **Step 5: Build everything**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target ecs
cmake --build --preset msvc-win64-vs2026-community --target Engine
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: all clean.

- [ ] **Step 6: Run the test sweep**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_aamode
./out/build/msvc-win64-vs2026-community/bin/Debug/test_aamode.exe
```
Expected: `All AA-mode tests passed.`

- [ ] **Step 7: Manual verification (human-run — this is graphics)**

Launch the editor. In the RenderStats panel's **Anti-aliasing** combo:
1. **Off** → aliased edges (baseline).
2. **FXAA** → smoothed edges, slightly soft textures (unchanged from before).
3. **SMAA** → smoothed edges with noticeably sharper textures/text than FXAA.
4. Resize the window across all three modes — no crash, no garbage (intermediate targets rebuild).
5. Set SMAA, restart the editor → it comes back as SMAA (persisted as `renderer.aaMode: 2`).
6. Hand-edit `engine_settings.json` to the legacy `"fxaa": true` (remove `aaMode`) → loads as FXAA.

- [ ] **Step 8: Commit**

```bash
git add src/engine/src/rendering/Renderer.h src/engine/src/rendering/Renderer.cpp src/editor/src/panels/RenderStatsPanel.cpp
git commit -m "feat(render): route SMAA through Renderer AA-mode switch + panel option"
```

---

## Self-Review Notes (spec coverage)

- Spec §1 settings model + migration → Tasks 1 (helper+test), 2 (enum, struct, load/save, sync, persist, panel).
- Spec §2 SmaaRenderPass (3 sub-passes, intermediate RTs, same socket) → Tasks 4 (resources), 5 (Render).
- Spec §3 vendored SMAA.hlsl assembled as a string + single preset config block → Tasks 3, 4.
- Spec §4 embedded AreaTex/SearchTex → Tasks 3, 4.
- Spec §5 Renderer AA-mode switch (Off/FXAA/SMAA, shared offscreen + fallback) → Task 6.
- Spec §6 RT_METRICS CB + unique-slot bindings + Vulkan offsets → Task 4.
- Spec §7 error handling (init-fail → null + one-shot warn fallback; resize rebuild) → Tasks 4, 5, 6.
- Spec §8/§9 testing: pure migration unit test (Task 1) + build gates + manual checklist (Task 6).
- Licensing: vendored LICENSE → Task 3.

## Known Risk Notes (not placeholders — concrete fallbacks)

- **Device-abstraction layer:** the plan uses `SMAA_CUSTOM_SL` with register-based samplers
  precisely because SMAA.hlsl's built-in `SMAA_HLSL_4` path declares samplers with legacy
  Effects state-block syntax that DXC rejects (verified by reading SMAA.hlsl lines 540-553).
  The prelude's custom macros mirror that block. If DXC still complains about a specific macro,
  the SMAA.hlsl HLSL4 block is the reference to mirror.
- **SMAA VS/PS signatures (verified against the submodule):** EdgeVS(tc, out off[3]);
  WeightVS(tc, out pix, out off[3]); BlendVS(tc, out off); `SMAALumaEdgeDetectionPS(tc, off[3],
  colorTex)`; `SMAABlendingWeightCalculationPS(tc, pix, off[3], edgesTex, areaTex, searchTex,
  float4(0))`; `SMAANeighborhoodBlendingPS(tc, off, colorTex, blendTex)`. The wrappers match
  these; if a future submodule bump changes them, SMAA.hlsl is the source of truth.
- **Lookup pitch macros (verified present):** `AREATEX_PITCH` = `AREATEX_WIDTH*2` (RG8),
  `SEARCHTEX_PITCH` = `SEARCHTEX_WIDTH` (R8). Used directly in Task 4 Step 2.
- **clearTextureFloat signature:** match the existing usage in the renderer if it differs
  (grep `clearTextureFloat` under `src/engine/src/rendering`) (Task 5 Step 2).
- **Gamma note:** SMAA luma edge detection wants gamma-corrected (non-sRGB) color input. The
  scene-color target format is whatever `EnsureSceneColor` uses (same input FXAA already
  consumes); if edges look wrong, confirm the scene-color isn't an sRGB-view mismatch.
