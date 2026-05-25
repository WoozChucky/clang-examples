# Atmospheric Fog Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add sun-driven exponential distance fog — thick/dark at night, near-zero at day — with the scene clear color matched to the fog color so the horizon is seamless.

**Architecture:** A single pure helper `ComputeFog(sunDir, FogSettings)` maps the directional sun's elevation to a fog color + density. The Renderer calls it once per frame to drive the scene clear ("sky") and stores the result; MeshRenderPass consumes the stored result, uploads it in the per-frame constant buffer, and the pixel shader blends geometry toward the fog color by `1 - exp(-density*dist)`. Editor exposes tuning via the existing Render Stats panel. No ECS/game changes.

**Tech Stack:** C++23, NVRHI (DX12/Vulkan), HLSL (compiled at runtime via DXC), GLM, ImGui (editor only).

**Verification:** No unit tests (per decision). Each task builds clean; final task is manual in-editor verification. Renderer shaders are runtime-compiled from string literals in `MeshRenderPass.cpp`, so a rebuild of `Engine.dll` is enough — but `editor.exe` does NOT hot-reload `Engine.dll`, so the editor must be restarted to see engine changes.

**Build note:** Commands below assume the configured preset dir `out/build/clang-win64-vs2026-community` exists. If it does not, run `cmake --preset clang-win64-vs2026-community` once first.

---

### Task 1: Fog settings + pure compute helper

**Files:**
- Create: `src/engine/src/rendering/Fog.h`
- Create: `src/engine/src/rendering/Fog.cpp`
- Modify: `src/engine/CMakeLists.txt` (add `Fog.cpp` after line 24, `src/rendering/RenderStats.cpp`)

- [ ] **Step 1: Create `Fog.h`**

```cpp
#pragma once

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "Engine.h"

// Editor-tunable fog parameters. Single instance lives in Fog.cpp and is
// exported from Engine.dll so the mesh pass (Engine.dll) and the editor panel
// (editor.exe) share ONE copy — same pattern as GetShadowSettings(). Touched
// only on the RenderThread (mesh pass + ImGui overlay run there).
struct FogSettings {
    bool      Enabled      = true;
    float     DayDensity   = 0.008f;                 // barely-there daytime haze
    float     NightDensity = 0.09f;                  // noticeable at night
    glm::vec3 DayColor     = glm::vec3(0.60f, 0.70f, 0.80f); // hazy blue-grey
    glm::vec3 NightColor   = glm::vec3(0.03f, 0.04f, 0.08f); // dark blue
};

ENGINE_API FogSettings& GetFogSettings();

// Resolved fog for one frame: the color used for BOTH the scene clear and the
// geometry blend, plus the exponential density.
struct FogFrame {
    glm::vec3 Color   = glm::vec3(0.0f);
    float     Density = 0.0f;
};

// Pure: maps the directional sun's elevation to fog color + density.
// elevation = clamp(-sunDir.y, 0, 1): 1 at noon, 0 at/below the horizon (night).
FogFrame ComputeFog(const glm::vec3& sunDir, const FogSettings& s);
```

- [ ] **Step 2: Create `Fog.cpp`**

```cpp
#include "Fog.h"

#include <glm/common.hpp> // glm::clamp, glm::mix

FogSettings& GetFogSettings()
{
    static FogSettings s;
    return s;
}

FogFrame ComputeFog(const glm::vec3& sunDir, const FogSettings& s)
{
    const float elevation = glm::clamp(-sunDir.y, 0.0f, 1.0f);
    FogFrame f;
    f.Density = glm::mix(s.NightDensity, s.DayDensity, elevation);
    f.Color   = glm::mix(s.NightColor,   s.DayColor,   elevation);
    return f;
}
```

- [ ] **Step 3: Register `Fog.cpp` in the engine CMake source list**

In `src/engine/CMakeLists.txt`, the line at 24 is `    src/rendering/RenderStats.cpp`. Add directly below it:

```cmake
    src/rendering/RenderStats.cpp
    src/rendering/Fog.cpp
```

- [ ] **Step 4: Build the engine**

Run: `cmake --build out/build/clang-win64-vs2026-community --target Engine`
Expected: build succeeds, no errors.

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/rendering/Fog.h src/engine/src/rendering/Fog.cpp src/engine/CMakeLists.txt
git commit -m "feat(fog): FogSettings + ComputeFog sun->fog mapping"
```

---

### Task 2: Renderer computes fog + drives scene clear color

**Files:**
- Modify: `src/engine/src/rendering/Renderer.h` (include, accessor, member)
- Modify: `src/engine/src/rendering/Renderer.cpp` (includes, compute before clear, clear color)

- [ ] **Step 1: Include `Fog.h` in `Renderer.h`**

In `src/engine/src/rendering/Renderer.h`, after the existing `#include "IRenderPass.h"` (line 10), add:

```cpp
#include "Fog.h"
```

- [ ] **Step 2: Add the `GetFrameFog()` accessor**

In `Renderer.h`, directly after the `GetShadowView()` accessor (line 127, `ShadowView& GetShadowView() { return m_ShadowView; }`), add:

```cpp
    const FogFrame& GetFrameFog() const { return m_FrameFog; }
```

- [ ] **Step 3: Add the `m_FrameFog` member**

In `Renderer.h`, directly after the `ShadowView m_ShadowView{};` member (line 160), add:

```cpp
    FogFrame m_FrameFog{}; // resolved each frame; drives clear + mesh-pass fog
```

- [ ] **Step 4: Include `ECS.h` in `Renderer.cpp`**

In `src/engine/src/rendering/Renderer.cpp`, after `#include "Renderer.h"` (line 1), add:

```cpp
#include "ECS.h" // world->Each, LightningComponent
```

- [ ] **Step 5: Compute fog before the clear and use it as the clear color**

In `Renderer.cpp`, replace this block (lines 216–218):

```cpp
                ZoneScopedN("RenderPasses");
                const auto sceneClear = nvrhi::Color(red, green, blue, 1.0f);
                nvrhi::utils::ClearColorAttachment(m_CommandList, sceneBuffer, 0, sceneClear);
```

with:

```cpp
                ZoneScopedN("RenderPasses");

                // Resolve sun-driven fog once per frame. The same color drives the
                // scene clear ("sky") and the geometry fog in MeshRenderPass, so the
                // horizon has no seam. elevation defaults to night if no sun exists.
                glm::vec3 sunDir(0.0f, 1.0f, 0.0f); // points up = below horizon = night default
                if (world) {
                    world->Each<TransformComponent, LightningComponent>(
                        [&](EntityId, const TransformComponent&, const LightningComponent& l) {
                            if (l.Type == LightningType::Directional) sunDir = glm::vec3(l.Direction);
                        });
                }
                const FogSettings& fogSettings = GetFogSettings();
                m_FrameFog = ComputeFog(sunDir, fogSettings);

                const auto sceneClear = fogSettings.Enabled
                    ? nvrhi::Color(m_FrameFog.Color.r, m_FrameFog.Color.g, m_FrameFog.Color.b, 1.0f)
                    : nvrhi::Color(red, green, blue, 1.0f);
                nvrhi::utils::ClearColorAttachment(m_CommandList, sceneBuffer, 0, sceneClear);
```

- [ ] **Step 6: Build the engine**

Run: `cmake --build out/build/clang-win64-vs2026-community --target Engine`
Expected: build succeeds. (Behavior so far: background color now shifts with the day/night cycle; geometry not yet fogged.)

- [ ] **Step 7: Commit**

```bash
git add src/engine/src/rendering/Renderer.h src/engine/src/rendering/Renderer.cpp
git commit -m "feat(fog): renderer resolves frame fog + drives sky clear color"
```

---

### Task 3: MeshRenderPass uploads fog + pixel shader applies it

**Files:**
- Modify: `src/engine/src/rendering/passes/MeshRenderPass.h` (`PerFrameCB` CPU struct)
- Modify: `src/engine/src/rendering/passes/MeshRenderPass.cpp` (both HLSL `cbuffer` blocks, CB fill, PS fog apply, include)

- [ ] **Step 1: Add fog fields to the CPU `PerFrameCB` struct**

In `src/engine/src/rendering/passes/MeshRenderPass.h`, replace the `PerFrameCB` struct (lines 39–49):

```cpp
    struct PerFrameCB
    {
        glm::mat4 P;   // Projection
        glm::mat4 VP;  // View-Projection
        glm::mat4 LightVP;            // shadow light view-projection
        DirectionalLight DirectionalLight;
        uint32_t PointLightCount = 0; // number of point lights in the structured buffer
        float    Ambient = 0.0f;
        int      ShadowEnabled = 0;
        float    ShadowBias = 0.0f;
    };
```

with:

```cpp
    struct PerFrameCB
    {
        glm::mat4 P;   // Projection
        glm::mat4 VP;  // View-Projection
        glm::mat4 LightVP;            // shadow light view-projection
        DirectionalLight DirectionalLight;
        uint32_t PointLightCount = 0; // number of point lights in the structured buffer
        float    Ambient = 0.0f;
        int      ShadowEnabled = 0;
        float    ShadowBias = 0.0f;
        glm::vec4 CameraPos{0.0f};    // xyz = camera world pos
        glm::vec4 Fog{0.0f};          // rgb = fog color, w = density
        int       FogEnabled = 0;
        float     _padFog[3]{};       // pad to 16-byte alignment
    };
```

- [ ] **Step 2: Add matching fields to BOTH HLSL `cbuffer PerFrame` blocks**

In `MeshRenderPass.cpp` there are two identical `cbuffer PerFrame : register(b0)` declarations — one in `MESH_VS_HLSL` (lines 28–38) and one in `MESH_PS_HLSL` (lines 110–120). They MUST stay byte-identical. In **each**, replace:

```hlsl
cbuffer PerFrame : register(b0)
{
    float4x4 uP;
    float4x4 uVP;
    float4x4 uLightVP;
    DirectionalLight uDirLight;
    uint  uPointLightCount;
    float uAmbient;
    int   uShadowEnabled;
    float uShadowBias;
};
```

with:

```hlsl
cbuffer PerFrame : register(b0)
{
    float4x4 uP;
    float4x4 uVP;
    float4x4 uLightVP;
    DirectionalLight uDirLight;
    uint  uPointLightCount;
    float uAmbient;
    int   uShadowEnabled;
    float uShadowBias;
    float4 uCameraPos; // xyz = camera world pos
    float4 uFog;       // rgb = fog color, w = density
    int    uFogEnabled;
    float3 _padFog;
};
```

- [ ] **Step 3: Include `Fog.h` in `MeshRenderPass.cpp`**

In `MeshRenderPass.cpp`, after `#include "Renderer.h"` (line 3), add:

```cpp
#include "Fog.h"
```

- [ ] **Step 4: Fill the fog CB fields**

In `MeshRenderPass.cpp`, find the per-frame CB fill ending at `perFrame.ShadowBias = GetShadowSettings().Bias;` (line 406). Directly after that line, add:

```cpp
        const FogFrame& fog = m_Renderer->GetFrameFog();
        perFrame.CameraPos  = glm::vec4(camPos, 1.0f);
        perFrame.Fog        = glm::vec4(fog.Color, fog.Density);
        perFrame.FogEnabled = GetFogSettings().Enabled ? 1 : 0;
```

(`camPos` is already in scope from line 392: `const glm::vec3 camPos = cam.Position;`.)

- [ ] **Step 5: Restructure the PS return paths and apply fog**

In `MeshRenderPass.cpp`, replace the tail of `main_ps` (lines 202–218), which currently is:

```hlsl
    if ((inst.Flags & OPT_UNLIT) != 0u)
    {
        float4 c = ((inst.Flags & OPT_SAMPLE_TEXTURE) != 0)
            ? uTexture.Sample(uSampler, i.UV)
            : float4(inst.BaseColor.rgb, inst.BaseColor.a);
        return c; // bypass lighting
    }

    float4 finalColor;
    if ((inst.Flags & OPT_SAMPLE_TEXTURE) != 0) {
        finalColor = uTexture.Sample(uSampler, i.UV);
        finalColor.rgb *= lighting;
    } else {
        finalColor.rgb = inst.BaseColor.rgb * lighting;
        finalColor.a = inst.BaseColor.a;
    }
    return finalColor;
```

with (single return so fog applies to lit AND unlit geometry):

```hlsl
    float4 finalColor;
    if ((inst.Flags & OPT_UNLIT) != 0u)
    {
        finalColor = ((inst.Flags & OPT_SAMPLE_TEXTURE) != 0)
            ? uTexture.Sample(uSampler, i.UV)
            : float4(inst.BaseColor.rgb, inst.BaseColor.a);
        // unlit: bypass lighting, but still receive atmospheric fog below
    }
    else if ((inst.Flags & OPT_SAMPLE_TEXTURE) != 0) {
        finalColor = uTexture.Sample(uSampler, i.UV);
        finalColor.rgb *= lighting;
    } else {
        finalColor.rgb = inst.BaseColor.rgb * lighting;
        finalColor.a = inst.BaseColor.a;
    }

    if (uFogEnabled != 0)
    {
        float dist = length(i.WorldPos - uCameraPos.xyz);
        float fogF = 1.0 - exp(-uFog.w * dist);
        finalColor.rgb = lerp(finalColor.rgb, uFog.rgb, fogF);
    }
    return finalColor;
```

- [ ] **Step 6: Build the engine**

Run: `cmake --build out/build/clang-win64-vs2026-community --target Engine`
Expected: build succeeds.

- [ ] **Step 7: Commit**

```bash
git add src/engine/src/rendering/passes/MeshRenderPass.h src/engine/src/rendering/passes/MeshRenderPass.cpp
git commit -m "feat(fog): mesh pass uploads fog CB + PS applies exp distance fog"
```

---

### Task 4: Editor UI for fog tuning

**Files:**
- Modify: `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`

- [ ] **Step 1: Include `Fog.h`**

In `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`, after `#include "RenderStats.h" ...` (line 5), add:

```cpp
#include "Fog.h" // resolved via the editor's Engine PUBLIC include dirs (same as RenderStats.h)
```

- [ ] **Step 2: Add the Fog controls**

In `RenderStatsPanel.cpp`, directly before `ImGui::End();` (line 36), add:

```cpp
    ImGui::Separator();
    ImGui::TextDisabled("Fog");
    FogSettings& fog = GetFogSettings();
    ImGui::Checkbox("Fog enabled", &fog.Enabled);
    ImGui::SliderFloat("Day density",   &fog.DayDensity,   0.0f, 0.05f, "%.4f");
    ImGui::SliderFloat("Night density", &fog.NightDensity, 0.0f, 0.30f, "%.3f");
    ImGui::ColorEdit3("Day color",   &fog.DayColor.x);
    ImGui::ColorEdit3("Night color", &fog.NightColor.x);
```

- [ ] **Step 3: Build the editor**

Run: `cmake --build out/build/clang-win64-vs2026-community --target editor`
Expected: build succeeds (this also rebuilds `Engine` if needed).

- [ ] **Step 4: Commit**

```bash
git add src/editor/src/rendering/imgui/RenderStatsPanel.cpp
git commit -m "feat(fog): editor Render Stats panel fog controls"
```

---

### Task 5: Manual verification in the editor

**Files:** none (verification only).

- [ ] **Step 1: Launch a fresh editor**

The running editor (if any) still has the old `Engine.dll` linked — close it first. Then run the built `editor.exe` from `out/build/clang-win64-vs2026-community/bin/<Config>/editor.exe`.

- [ ] **Step 2: Observe the day/night cycle (default 10s)**

Confirm all of:
- At **night** the scene is fogged and dark; distant geometry fades into the background.
- At **midday** fog is near-absent; scene is clear.
- The transition between the two is smooth (no popping).
- The **horizon has no seam** — the background clear color and the color distant geometry fades to agree.

- [ ] **Step 3: Exercise the Fog panel**

Open the "Render Stats" panel. Toggle `Fog enabled` (geometry + sky fog turn off; clear falls back to the legacy animated color). Drag `Night density` up/down and the color pickers — confirm live changes.

- [ ] **Step 4: Confirm existing tests still pass**

Run:
```bash
cmake --build out/build/clang-win64-vs2026-community --target test_ecs
./out/build/clang-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `All ECS tests passed.`

---

## Notes

- No `Game.h`/`ECS.h` struct changes → no `GAME_API_VERSION` bump, no `ecs.dll` rebuild. Only a normal rebuild + editor restart is needed.
- The legacy animated `red/green/blue` clear in `RenderThread.cpp` is intentionally left in place as the disabled-fog fallback; not removed.
