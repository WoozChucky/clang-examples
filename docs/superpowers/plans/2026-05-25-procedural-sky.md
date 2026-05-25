# Procedural Sky + Sun/Moon Discs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A full-screen `SkyRenderPass` that paints a day/night gradient sky plus moving sun and moon discs (sun position from the existing day/night cycle, moon opposite), tunable via editor `SkySettings`; plus a documentation pass updating README/CLAUDE for the deferred pipeline + fog/day-night/sky.

**Architecture:** A new pass after `LightingRenderPass`, full-screen triangle at the far plane with `depthFunc=LessOrEqual` + no depth-write so it only fills sky pixels (depth==1). Its PS reconstructs the view ray (inverse view-proj), builds a day/night gradient by sun elevation, and adds sun/moon discs via `dot(rayDir, ±sunDir)`. Appearance comes from an engine-side `SkySettings` global (like `FogSettings`); the sun direction is read from the ECS directional light.

**Tech Stack:** C++23, NVRHI deferred renderer (HLSL string literals), GLM, ImGui.

**Spec:** `docs/superpowers/specs/2026-05-25-procedural-sky-design.md`

**Build/verify:** Preset `msvc-win64-vs2026-enterprise`. `cmake --build out/build/msvc-win64-vs2026-enterprise --target Engine` / `--target editor`. No unit tests for renderer visuals (project norm); verify by build-clean + manual cycle scrub. No `ECS.h` change → normal build, no restart-for-layout needed (editor reload of `Engine.dll` requires relaunching the editor exe as usual).

**Branch:** start a fresh branch off `main`: `git checkout main && git checkout -b feat/procedural-sky`. Never stage the pre-existing `src/engine/src/rendering/backends/RendererBackendDX12.cpp` change.

---

## File structure
- `src/engine/src/rendering/Sky.{h,cpp}` (new) — `SkySettings` + `GetSkySettings()` (engine global, `Fog.{h,cpp}` pattern).
- `src/engine/src/rendering/passes/SkyRenderPass.{h,cpp}` (new) — the full-screen sky pass.
- `src/engine/src/rendering/Renderer.cpp` — register the pass after lighting (Init + InitForSwap).
- `src/engine/CMakeLists.txt` — add the two new `.cpp`s.
- `src/editor/src/rendering/imgui/RenderStatsPanel.cpp` — Sky tuning section.
- `README.md`, `CLAUDE.md` (+ `docs/ECS_Threading_Architecture.md` if a claim is now false) — documentation pass.

---

## Task 1: SkySettings (engine global)

**Files:**
- Create: `src/engine/src/rendering/Sky.h`
- Create: `src/engine/src/rendering/Sky.cpp`
- Modify: `src/engine/CMakeLists.txt`

- [ ] **Step 1: Create `Sky.h`** (mirror `Fog.h`: `#include "Engine.h"` for `ENGINE_API`)
```cpp
#pragma once

#include <glm/vec3.hpp>

#include "Engine.h"

// Editor-tunable procedural-sky parameters. Single instance in Sky.cpp, exported
// from Engine.dll (same pattern as GetFogSettings). Touched only on the RenderThread.
struct SkySettings {
    bool      Enabled       = true;
    glm::vec3 DayZenith     = glm::vec3(0.20f, 0.40f, 0.85f);
    glm::vec3 DayHorizon    = glm::vec3(0.70f, 0.80f, 0.95f);
    glm::vec3 NightZenith   = glm::vec3(0.01f, 0.02f, 0.06f);
    glm::vec3 NightHorizon  = glm::vec3(0.04f, 0.05f, 0.12f);
    glm::vec3 SunColor      = glm::vec3(1.00f, 0.95f, 0.80f);
    float     SunRadiusDeg  = 3.0f;
    float     SunGlow       = 64.0f;   // halo falloff exponent (higher = tighter)
    glm::vec3 MoonColor     = glm::vec3(0.80f, 0.85f, 1.00f);
    float     MoonRadiusDeg = 2.5f;
    float     MoonGlow      = 128.0f;
};

ENGINE_API SkySettings& GetSkySettings();
```

- [ ] **Step 2: Create `Sky.cpp`**
```cpp
#include "Sky.h"

SkySettings& GetSkySettings()
{
    static SkySettings s;
    return s;
}
```

- [ ] **Step 3: Register in CMake**
In `src/engine/CMakeLists.txt`, next to `src/rendering/Fog.cpp`, add:
```cmake
    src/rendering/Sky.cpp
```

- [ ] **Step 4: Build**
`cmake --build out/build/msvc-win64-vs2026-enterprise --target Engine`
Expected: clean.

- [ ] **Step 5: Commit**
```bash
git add src/engine/src/rendering/Sky.h src/engine/src/rendering/Sky.cpp src/engine/CMakeLists.txt
git commit -m "feat(sky): SkySettings engine global"
```

---

## Task 2: SkyRenderPass

**Files:**
- Create: `src/engine/src/rendering/passes/SkyRenderPass.h`
- Create: `src/engine/src/rendering/passes/SkyRenderPass.cpp`
- Modify: `src/engine/CMakeLists.txt`
- Modify: `src/engine/src/rendering/Renderer.cpp` (register after lighting)

Read `src/engine/src/rendering/passes/LightingRenderPass.{h,cpp}` first — `SkyRenderPass` is a simpler sibling (full-screen triangle, `draw(3)`, no input layout, a single constant buffer, no textures). Reuse its Initialize/Render structure and the `nvrhi::VulkanBindingOffsets` block.

- [ ] **Step 1: Create `SkyRenderPass.h`**
```cpp
#pragma once
#include <nvrhi/nvrhi.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <cstdint>
#include "IRenderPass.h"

class Renderer;

// Full-screen procedural sky drawn into sky pixels (far-plane depth test):
// day/night gradient + sun disc + moon disc. Runs after LightingRenderPass.
class SkyRenderPass : public IRenderPass {
public:
    bool Initialize(nvrhi::IDevice* device, Renderer* renderer) override;
    void Render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* frameBuffer,
                SimulationSnapshot& snapshot, const ECS* world,
                double deltaTime, FrameAllocator* frameAllocator) override;
    void Shutdown() override;
    void OnResize(uint32_t width, uint32_t height) override;
private:
    struct SkyFrameCB {
        glm::mat4 InvViewProj;  // 64
        glm::vec4 CameraPos;    // xyz
        glm::vec4 SunDir;       // xyz = light travel direction
        glm::vec4 DayZenith;
        glm::vec4 DayHorizon;
        glm::vec4 NightZenith;
        glm::vec4 NightHorizon;
        glm::vec4 SunColor;     // rgb; w = glow exponent
        glm::vec4 MoonColor;    // rgb; w = glow exponent
        glm::vec4 Disc;         // x=sunCosOuter y=sunCosInner z=moonCosOuter w=moonCosInner
    };
    static_assert(sizeof(SkyFrameCB) % 16 == 0, "SkyFrameCB must be 16-byte aligned");

    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;
    nvrhi::ShaderHandle m_VS, m_PS;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BufferHandle m_FrameCB;
};
```
(Layout: 64 + 9×16 = 208 bytes, 16-aligned.)

- [ ] **Step 2: Shaders (string literals in SkyRenderPass.cpp)**
```cpp
static const char* SKY_VS_HLSL = R"(
struct VSOut { float4 PosH:SV_POSITION; float2 UV:TEXCOORD0; };
VSOut main_vs(uint vid : SV_VertexID){
    VSOut o;
    o.UV = float2((vid << 1) & 2, vid & 2);
    // z = 1.0 (far plane) so the LessOrEqual depth test only passes on sky pixels.
    o.PosH = float4(o.UV * float2(2,-2) + float2(-1,1), 1.0, 1.0);
    return o;
}
)";

static const char* SKY_PS_HLSL = R"(
cbuffer SkyCB : register(b0) {
    float4x4 uInvViewProj;
    float4 uCameraPos;
    float4 uSunDir;
    float4 uDayZenith;
    float4 uDayHorizon;
    float4 uNightZenith;
    float4 uNightHorizon;
    float4 uSunColor;   // w = glow exponent
    float4 uMoonColor;  // w = glow exponent
    float4 uDisc;       // x sunCosOuter, y sunCosInner, z moonCosOuter, w moonCosInner
};
struct PSIn { float4 PosH:SV_POSITION; float2 UV:TEXCOORD0; };

float4 main_ps(PSIn i) : SV_Target {
    // Screen UV -> NDC (top-left origin: uv.y=0 is top -> ndc.y=+1).
    float2 ndc = float2(i.UV.x * 2.0 - 1.0, 1.0 - i.UV.y * 2.0);
    float4 far = mul(uInvViewProj, float4(ndc, 1.0, 1.0));
    float3 rayDir = normalize(far.xyz / far.w - uCameraPos.xyz);

    float elev = saturate(-uSunDir.y);          // 1 noon, 0 horizon/night
    float t    = saturate(rayDir.y);            // horizon -> zenith
    float3 dayCol   = lerp(uDayHorizon.rgb,   uDayZenith.rgb,   t);
    float3 nightCol = lerp(uNightHorizon.rgb, uNightZenith.rgb, t);
    float3 sky = lerp(nightCol, dayCol, elev);

    // Sun: sky direction of the sun is -lightDir.
    float sdot = dot(rayDir, -uSunDir.xyz);
    float sunDisc = smoothstep(uDisc.x, uDisc.y, sdot);
    float sunHalo = pow(saturate(sdot), uSunColor.w) * 0.5;
    sky += uSunColor.rgb * (sunDisc + sunHalo);

    // Moon: opposite the sun (+lightDir).
    float mdot = dot(rayDir, uSunDir.xyz);
    float moonDisc = smoothstep(uDisc.z, uDisc.w, mdot);
    float moonHalo = pow(saturate(mdot), uMoonColor.w) * 0.3;
    sky += uMoonColor.rgb * (moonDisc + moonHalo);

    return float4(sky, 1.0);
}
)";
```
If the `ndc.y` sign produces an upside-down sky at verify time (sun appears at the bottom when it should be up), flip it to `1.0 - i.UV.y*2.0` → `i.UV.y*2.0 - 1.0`. Note this and confirm during Task 5 (the lighting/G-buffer passes use the same `uv*float2(2,-2)+(-1,1)` full-screen VS, so this convention should match).

- [ ] **Step 3: Implement `SkyRenderPass.cpp` (C++)**
- `Initialize`: store device/renderer; create `m_VS`/`m_PS` via `m_Renderer->CreateShader(... "main_vs","vs_6_1")` / `"main_ps","ps_6_1"`. Create `m_FrameCB` = constant buffer of `sizeof(SkyFrameCB)` (copy the CB-creation BufferDesc from LightingRenderPass). Binding layout = ONLY `nvrhi::BindingLayoutItem::ConstantBuffer(0)` + the same `nvrhi::VulkanBindingOffsets` block LightingRenderPass uses. No input layout, no textures.
- `Render`:
  - `if (!GetSkySettings().Enabled) return;` (include `"Sky.h"`).
  - Lazily build `m_Pipeline` against `frameBuffer->getFramebufferInfo()`: `pso.VS/PS`, `bindingLayouts={m_BindingLayout}`, no inputLayout, `primType=TriangleList`, `rasterState.cullMode=None`, blend disabled on RT0, and the far-plane depth test:
    ```cpp
    pso.renderState.depthStencilState.depthTestEnable = true;
    pso.renderState.depthStencilState.depthWriteEnable = false;
    pso.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;
    ```
  - Read the sun direction from the ECS like the lighting pass does (copy that small `Each<TransformComponent, LightningComponent>` directional-light scan from LightingRenderPass.cpp; take the directional light's `Direction`). Default `glm::vec4(0,-1,0,0)` if none.
  - Build the CB:
    ```cpp
    const CameraView& cam = m_Renderer->GetActiveCamera();
    const SkySettings& s = GetSkySettings();
    SkyFrameCB cb{};
    cb.InvViewProj = glm::inverse(cam.Projection * cam.View);
    cb.CameraPos   = glm::vec4(cam.Position, 1.0f);
    cb.SunDir      = sunDir; // glm::vec4 from the gather
    cb.DayZenith    = glm::vec4(s.DayZenith, 0.0f);
    cb.DayHorizon   = glm::vec4(s.DayHorizon, 0.0f);
    cb.NightZenith  = glm::vec4(s.NightZenith, 0.0f);
    cb.NightHorizon = glm::vec4(s.NightHorizon, 0.0f);
    cb.SunColor    = glm::vec4(s.SunColor,  s.SunGlow);
    cb.MoonColor   = glm::vec4(s.MoonColor, s.MoonGlow);
    const float soft = glm::radians(0.5f);
    const float sunR  = glm::radians(s.SunRadiusDeg);
    const float moonR = glm::radians(s.MoonRadiusDeg);
    cb.Disc = glm::vec4(cosf(sunR + soft), cosf(sunR), cosf(moonR + soft), cosf(moonR));
    commandList->writeBuffer(m_FrameCB, &cb, sizeof(cb));
    ```
    (Include `<glm/gtc/matrix_inverse.hpp>` or `<glm/matrix.hpp>` for `glm::inverse`, and `<glm/trigonometric.hpp>` for `glm::radians`; `cosf` from `<cmath>`. Check which GLM headers the neighboring passes already pull in.)
  - Binding set: `nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameCB)`. Graphics state: `state.framebuffer=frameBuffer; state.pipeline=m_Pipeline; state.bindings={set}; state.viewport.addViewportAndScissorRect(frameBuffer->getFramebufferInfo().getViewport());` no vertex/index buffers; `nvrhi::DrawArguments a; a.vertexCount=3; commandList->draw(a);` (wrap in `beginMarker("SkyRenderPass")`/`endMarker` like the others).
- `Shutdown`: null handles. `OnResize`: `m_Pipeline=nullptr;`.

- [ ] **Step 4: CMake**
In `src/engine/CMakeLists.txt`, next to `LightingRenderPass.cpp`, add:
```cmake
    src/rendering/passes/SkyRenderPass.cpp
```

- [ ] **Step 5: Register after lighting (Renderer.cpp, BOTH Init and InitForSwap)**
Add `#include "passes/SkyRenderPass.h"`. Immediately AFTER the `LightingRenderPass` registration block (the `AddRenderPass(std::move(lightingPass))` / equivalent), add the same Initialize-checked block for `SkyRenderPass` so the order becomes `... Lighting -> Sky -> Primitive -> ...`:
```cpp
    auto skyPass = std::make_unique<SkyRenderPass>();
    if (!skyPass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize SkyRenderPass");
        return false;
    }
    AddRenderPass(std::move(skyPass));
```
Do it in both `Init` and `InitForSwap`, positioned right after the lighting pass and before the primitive (grid) pass.

- [ ] **Step 6: Build + run**
`cmake --build out/build/msvc-win64-vs2026-enterprise --target editor`
Expected: clean. (Full visual check is Task 5.)

- [ ] **Step 7: Commit**
```bash
git add src/engine/src/rendering/passes/SkyRenderPass.h src/engine/src/rendering/passes/SkyRenderPass.cpp src/engine/CMakeLists.txt src/engine/src/rendering/Renderer.cpp
git commit -m "feat(sky): SkyRenderPass — gradient + sun/moon discs at far plane"
```

---

## Task 3: Editor Sky tuning section

**Files:**
- Modify: `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`

Mirror the existing Fog section in this file.

- [ ] **Step 1: Include Sky.h**
After the existing `#include "Fog.h"` line, add:
```cpp
#include "Sky.h"
```

- [ ] **Step 2: Add the Sky section**
Immediately before the final `ImGui::End();` in `DrawRenderStatsPanel`, add:
```cpp
    ImGui::Separator();
    ImGui::TextDisabled("Sky");
    SkySettings& sky = GetSkySettings();
    ImGui::Checkbox("Sky enabled", &sky.Enabled);
    ImGui::ColorEdit3("Day zenith",    &sky.DayZenith.x);
    ImGui::ColorEdit3("Day horizon",   &sky.DayHorizon.x);
    ImGui::ColorEdit3("Night zenith",  &sky.NightZenith.x);
    ImGui::ColorEdit3("Night horizon", &sky.NightHorizon.x);
    ImGui::ColorEdit3("Sun color",     &sky.SunColor.x);
    ImGui::SliderFloat("Sun radius (deg)",  &sky.SunRadiusDeg, 0.5f, 15.0f, "%.1f");
    ImGui::SliderFloat("Sun glow",          &sky.SunGlow, 1.0f, 512.0f, "%.0f");
    ImGui::ColorEdit3("Moon color",    &sky.MoonColor.x);
    ImGui::SliderFloat("Moon radius (deg)", &sky.MoonRadiusDeg, 0.5f, 15.0f, "%.1f");
    ImGui::SliderFloat("Moon glow",         &sky.MoonGlow, 1.0f, 512.0f, "%.0f");
```

- [ ] **Step 3: Build the editor**
`cmake --build out/build/msvc-win64-vs2026-enterprise --target editor`
Expected: clean.

- [ ] **Step 4: Commit**
```bash
git add src/editor/src/rendering/imgui/RenderStatsPanel.cpp
git commit -m "feat(sky): editor Sky tuning section"
```

---

## Task 4: Documentation pass

**Files:**
- Modify: `README.md`
- Modify: `CLAUDE.md`
- Modify (only if a claim is now false): `docs/ECS_Threading_Architecture.md`

Bring the docs in line with the merged deferred pipeline + fog/day-night/sky. Read each file's current renderer text first; keep edits accurate to the code.

- [ ] **Step 1: README.md — renderer / render-passes sections**
The "Render passes" list (~lines 165-167) currently reads `PrimitiveRenderPass`, `MeshRenderPass`, `UiRenderPass`, and the "Renderer" blurb (~lines 130-167) describes a forward flow. Replace the pass list + flow with the deferred reality. The pass list should become (keep the file's existing bullet style/voice):
```
- `ShadowDepthPass` – directional shadow depth map (fit to the visible-mesh AABB).
- `GBufferFillPass` – opaque geometry into a G-buffer (albedo / world-normal / world-position MRT + depth).
- `LightingRenderPass` – full-screen deferred lighting: directional + point lights, 3x3 PCF shadows, and exponential distance fog, reading the G-buffer.
- `SkyRenderPass` – full-screen procedural sky: day/night gradient + sun/moon discs (far-plane depth test).
- `PrimitiveRenderPass` – procedural primitives (e.g., the ground grid).
- `OutlineRenderPass` – selection silhouette. `DebugRenderPass` – gizmos. `UiRenderPass` – text/quads.
```
And update the surrounding prose: the renderer is **deferred** (G-buffer geometry pass + full-screen lighting), with **fog** (`Fog.{h,cpp}` / `GetFogSettings`), a **day/night cycle** (`DayNightSystem` in `game.cpp` driving the sun light + writing `AtmosphereStateComponent`; tunables in `DayNightConfigComponent`), and a **procedural sky** (`Sky.{h,cpp}` / `GetSkySettings`). Remove the `MeshRenderPass` line/mentions. Do not invent details — match the code.

- [ ] **Step 2: CLAUDE.md — Renderer (Engine) section + ECSCommandProcessor location**
- In the "Renderer (Engine)" paragraph (~line 96) that lists `PrimitiveRenderPass`, `MeshRenderPass`, `UiRenderPass` as the gameplay passes, replace with the deferred pipeline description + the pass order from Step 1, and mention `FogSettings`/`SkySettings` (editor-tunable, `GetFogSettings()`/`GetSkySettings()`) and the day/night components.
- Fix the stale location: CLAUDE.md states the `ECSCommandProcessor` is "in `ApplicationContext.h`" (in the "ECS command pattern" section and the "adding a new component type" steps). It is actually in **`src/common/include/ECSCommands.h`**. Update those references to point to `ECSCommands.h`.
- If CLAUDE.md mentions the registered component set or `MeshRenderPass` anywhere else, correct it (e.g. note `DayNightConfigComponent` / `AtmosphereStateComponent` exist; the inspector/mesh-pass mentions).

- [ ] **Step 3: docs/ECS_Threading_Architecture.md — light touch**
Grep it for `MeshRenderPass` / renderer-pass claims. The snapshot/command threading model is unchanged, so only fix a sentence if it is now factually wrong (e.g. a pass name). If nothing is wrong, leave the file untouched and note that.

- [ ] **Step 4: Sanity check the docs**
Re-read the edited sections; confirm every pass/file/type named actually exists in the tree (`Fog.h`, `Sky.h`, `GBufferFillPass.*`, `LightingRenderPass.*`, `SkyRenderPass.*`, `DayNightConfigComponent`, `AtmosphereStateComponent`, `ECSCommands.h`). No references to the removed `MeshRenderPass`.

- [ ] **Step 5: Commit**
```bash
git add README.md CLAUDE.md docs/ECS_Threading_Architecture.md
git commit -m "docs: update README/CLAUDE for deferred pipeline + fog/day-night/sky"
```
(If you did not touch the threading doc, omit it from the `git add`.)

---

## Task 5: Verification

**Files:** none (manual; fix-only if issues).

- [ ] **Step 1: Build + run**
`cmake --build out/build/msvc-win64-vs2026-enterprise --target editor`; launch `out/build/msvc-win64-vs2026-enterprise/bin/Debug/editor.exe` fresh.

- [ ] **Step 2: Observe the sky**
Scrub the day/night cycle and confirm: a sun disc rises, crosses, and sets; a moon disc is opposite it (rises as the sun sets); the gradient shifts day↔night; the **sun is up (not flipped)** — if the disc is mirrored vertically, fix the `ndc.y` sign in the sky PS (Task 2 Step 2) and rebuild. Confirm foreground geometry **occludes** the sky (the grid/meshes are drawn over it — far-plane depth test working), and disabling "Sky enabled" reverts to the flat sky.

- [ ] **Step 3: Tune via the Sky panel**
Render Stats → Sky: change gradient colors, sun/moon color, radius, glow — confirm live changes.

- [ ] **Step 4: Regression**
```
cmake --build out/build/msvc-win64-vs2026-enterprise --target test_ecs
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_ecs.exe
```
Expected: `All ECS tests passed.`

---

## Notes
- No `ECS.h`/`Game.h` change → normal build; relaunch the editor exe to pick up the new `Engine.dll`.
- The far-plane depth test assumes `GBufferFillPass` remains the sole scene-depth writer and the standard depth convention (clear to 1.0 = far). Both hold today.
- Disc colors come from `SkySettings` (always bright when the body is above the horizon), independent of the day/night-faded directional light color.
- Vulkan can't be run on this machine; the pass uses the same single-CB binding + Vulkan offsets pattern as the other passes (no new collision risk — only `b0`), but confirm on Vulkan HW when available.
