# Day/Night Cycle Overhaul Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cap daytime brightness (no blow-out), make the cycle longer + smoother, and add a varying cool "ambient moon" at night — driven by `game.dll`'s `DayNightSystem`, tunable from an editor ImGui panel, consumed by the deferred `LightingRenderPass`.

**Architecture:** `DayNightSystem` (game) reads tunables from `DayNightConfigComponent` (singleton), computes the sun light (orbit unchanged; brightness capped + faded at night) and a moon/day ambient color, and writes that color into a new `AtmosphereStateComponent` singleton. The lighting pass reads that singleton and uses it as the ambient term (replacing the sun-tinted hardcoded ambient). The editor edits the config singleton via an `ECSCommand`.

**Tech Stack:** C++23, ECS (`ecs.dll`, X-macro registered components), NVRHI deferred lighting pass (HLSL string literals), GLM, ImGui (editor).

**Spec:** `docs/superpowers/specs/2026-05-25-daynight-cycle-design.md`

**Build/verify:** Preset `msvc-win64-vs2026-enterprise`. Because this changes `ECS.h`, you must rebuild `ecs`, `Engine`, `game`, and `editor`, and **restart the editor** (the running editor has the old ECS layout linked). No unit tests for renderer/gameplay visuals (project norm) — verification is build-clean + a manual cycle scrub. `test_ecs` must still pass.
- Rebuild everything: `cmake --build out/build/msvc-win64-vs2026-enterprise --target editor` (pulls ecs/Engine), then `--target game` and `--target test_ecs` as needed.

**Branch:** `feat/atmospheric-fog` (current). Never stage the pre-existing `src/engine/src/rendering/backends/RendererBackendDX12.cpp` change.

---

## File structure
- `src/common/include/ECS.h` — extend `DayNightConfigComponent`; add `AtmosphereStateComponent`; add it to the X-macro.
- `src/common/include/ECSCommands.h` — register `DayNightConfigComponent` in the two `ECSCommandProcessor` dispatch branches (so ImGui can modify it).
- `src/game/src/game.cpp` — seed the new singleton at startup; rewrite `DayNightSystem`.
- `src/engine/src/rendering/passes/LightingRenderPass.{h,cpp}` — ambient-color CB field + shader change + read the singleton.
- `src/editor/src/rendering/imgui/DayNightPanel.{h,cpp}` (new) + register it where editor panels are drawn + `src/editor/CMakeLists.txt`.

---

## Task 1: ECS components + command registration + startup seed

**Files:**
- Modify: `src/common/include/ECS.h`
- Modify: `src/common/include/ECSCommands.h`
- Modify: `src/game/src/game.cpp` (startup seed only)

- [ ] **Step 1: Extend `DayNightConfigComponent` and add `AtmosphereStateComponent`**
In `src/common/include/ECS.h`, replace the line `struct DayNightConfigComponent { float CycleSeconds = 10.0f; };` with:
```cpp
struct DayNightConfigComponent {
    float     CycleSeconds  = 60.0f;                 // full day length (was 10)
    float     DayBrightness = 1.0f;                  // peak sun brightness (cap <= 1.0, no blow-out)
    float     MoonIntensity = 0.15f;                 // night ambient strength
    float     TwilightWidth = 0.25f;                 // smoothstep band for dawn/dusk easing (elevation units)
    float     DayAmbient    = 0.08f;                 // neutral daytime ambient floor
    glm::vec3 MoonColor     = glm::vec3(0.10f, 0.14f, 0.26f); // cool blue night fill
};

// Computed per-tick by DayNightSystem (game), read by the deferred LightingRenderPass.
struct AtmosphereStateComponent {
    glm::vec4 AmbientColor = glm::vec4(0.08f, 0.08f, 0.08f, 1.0f); // rgb = omnidirectional ambient
};
```
(`ECS.h` already includes GLM; `glm::vec3`/`glm::vec4` are used by other components.)

- [ ] **Step 2: Register `AtmosphereStateComponent` in the X-macro**
In the `ECS_FOR_EACH_REGISTERED_COMPONENT(X)` macro, add a line (e.g. right after `X(DayNightConfigComponent) \`):
```cpp
    X(AtmosphereStateComponent) \
```
(`DayNightConfigComponent` is already in the macro — leave it.)

- [ ] **Step 3: Register `DayNightConfigComponent` in the command processor (for ImGui editing)**
In `src/common/include/ECSCommands.h`, in `ApplyComponentCommand`, add a branch alongside the others (e.g. after the `SunMarker` branch):
```cpp
        } else if (componentData.Type == std::type_index(typeid(DayNightConfigComponent))) {
            if (auto* cfg = componentData.Get<DayNightConfigComponent>()) {
                world.AddComponent(entity, *cfg); // AddComponent updates if present
            }
```
And in `RemoveComponentByType`, add:
```cpp
        } else if (typeIndex == std::type_index(typeid(DayNightConfigComponent))) {
            world.RemoveComponent<DayNightConfigComponent>(entity);
```
(`AtmosphereStateComponent` is game-written/renderer-read only — it does NOT need command registration.)

- [ ] **Step 4: Seed the `AtmosphereStateComponent` singleton at startup**
In `src/game/src/game.cpp`, find the startup seeding block (the `GameStateId::Uninitialized` case) where `g_GameState->World.SetSingleton(DayNightConfigComponent{});` is called. Directly after that line add:
```cpp
	        g_GameState->World.SetSingleton(AtmosphereStateComponent{});
```

- [ ] **Step 5: Build everything + run test_ecs**
```
cmake --build out/build/msvc-win64-vs2026-enterprise --target editor
cmake --build out/build/msvc-win64-vs2026-enterprise --target game
cmake --build out/build/msvc-win64-vs2026-enterprise --target test_ecs
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_ecs.exe
```
Expected: all build clean; `All ECS tests passed.` (No behavior change yet — components exist + registered, the new singleton is seeded but nothing reads/writes its computed value.)

- [ ] **Step 6: Commit**
```bash
git add src/common/include/ECS.h src/common/include/ECSCommands.h src/game/src/game.cpp
git commit -m "feat(daynight): config tunables + AtmosphereStateComponent + command reg"
```

---

## Task 2: Lighting pass consumes the ambient color

**Files:**
- Modify: `src/engine/src/rendering/passes/LightingRenderPass.h`
- Modify: `src/engine/src/rendering/passes/LightingRenderPass.cpp`

Replace the sun-tinted hardcoded ambient with a dedicated ambient color read from `AtmosphereStateComponent`. Until Task 3 writes a computed value, the seeded default (0.08 neutral) is used — scene stays near its current look.

- [ ] **Step 1: Update `LightFrameCB` (LightingRenderPass.h)**
Replace the `LightFrameCB` struct (currently has `float Ambient` + `int _pad[3]`) with:
```cpp
    struct LightFrameCB {
        glm::mat4 LightVP;
        DirectionalLight Dir;
        glm::vec4 CameraPos;   // xyz
        glm::vec4 Fog;         // rgb=color, w=density
        glm::vec4 AmbientColor; // rgb = omnidirectional ambient (replaces uAmbient*sunColor)
        uint32_t  PointLightCount; int ShadowEnabled; float ShadowBias; int FogEnabled;
    };
    static_assert(sizeof(LightFrameCB) % 16 == 0, "LightFrameCB must be 16-byte aligned");
```
(Layout: 64 + 32 + 16 + 16 + 16 + 16 = 160 bytes, 16-aligned. `Ambient` float removed; `AmbientColor` vec4 added; the final 4 scalars fill one 16-byte row so no pad needed.)

- [ ] **Step 2: Update the HLSL cbuffer (LightingRenderPass.cpp)**
In the pixel-shader string, replace the `cbuffer PerFrame` block with (note `uAmbient` removed, `uAmbientColor` added — must stay byte-identical to the C++ struct):
```hlsl
cbuffer PerFrame : register(b0) {
    float4x4 uLightVP;
    DirectionalLight uDir;
    float4 uCameraPos;
    float4 uFog;
    float4 uAmbientColor;
    uint uPointLightCount; int uShadowEnabled; float uShadowBias; int uFogEnabled;
};
```

- [ ] **Step 3: Change the shader ambient term (LightingRenderPass.cpp)**
In `main_ps`, replace:
```hlsl
    float ambient = uAmbient;
    float3 lighting = ambient * uDir.Color.rgb;
```
with:
```hlsl
    float3 lighting = uAmbientColor.rgb;
```
Leave the diffuse, point-light loop, shadow, and fog code unchanged.

- [ ] **Step 4: Fill `AmbientColor` from the singleton (LightingRenderPass.cpp)**
In `Render`, in the CB-fill block, remove the line `cb.Ambient = 0.1f; ...` and add (the pass already has the `world` pointer used for light gather):
```cpp
    glm::vec4 ambientColor(0.08f, 0.08f, 0.08f, 1.0f); // fallback if the singleton isn't present yet
    if (world) {
        if (const auto* atm = world->GetSingleton<AtmosphereStateComponent>())
            ambientColor = atm->AmbientColor;
    }
    cb.AmbientColor = ambientColor;
```
(`AtmosphereStateComponent` comes from `ECS.h`, already reachable here — the pass calls `world->Each<TransformComponent, LightningComponent>` above. If the type isn't visible, add `#include "ECS.h"`.)

- [ ] **Step 5: Build the engine**
`cmake --build out/build/msvc-win64-vs2026-enterprise --target Engine`
Expected: clean (the `LightFrameCB` static_assert compiles → layout is 160 bytes). If the static_assert fails, do NOT fudge the number — fix the field layout to match the HLSL.

- [ ] **Step 6: Commit**
```bash
git add src/engine/src/rendering/passes/LightingRenderPass.h src/engine/src/rendering/passes/LightingRenderPass.cpp
git commit -m "feat(daynight): lighting pass uses AtmosphereStateComponent ambient color"
```

---

## Task 3: DayNightSystem — capped day, smooth cycle, moon ambient

**Files:**
- Modify: `src/game/src/game.cpp`

Rewrite the `DayNightSystem::Update` body. Read the current one first (class `DayNightSystem`). Keep the sun **direction** orbit exactly as-is (fog + shadows depend on it). Change brightness/color (capped + eased + faded), and compute + write the ambient color.

- [ ] **Step 1: Replace the `DayNightSystem::Update` body**
Replace the contents of `DayNightSystem::Update` with:
```cpp
    void Update(SystemContext& ctx) override {
        // Tunables (fallback to defaults if the config singleton is missing).
        DayNightConfigComponent cfg{};
        if (const auto* c = ctx.world.GetSingleton<DayNightConfigComponent>()) cfg = *c;

        const float cycle = glm::max(cfg.CycleSeconds, 0.001f);
        const double gameTime = ctx.gameTime;
        const auto phase = static_cast<float>(std::fmod(gameTime, static_cast<double>(cycle)) / static_cast<double>(cycle));
        const float theta = phase * 6.28318530718f;
        const glm::vec3 dir = glm::normalize(glm::vec3(0.0f, -cosf(theta), sinf(theta)));

        const float elevation = glm::clamp(-dir.y, 0.0f, 1.0f); // 1 = noon, 0 = at/below horizon
        const float nightDepth = glm::clamp(dir.y, 0.0f, 1.0f);  // 0 = horizon, 1 = deep midnight
        const float tw = glm::max(cfg.TwilightWidth, 0.001f);

        // Sun: warm at the horizon, white high up; brightness eased + capped, faded to ~0 at night.
        const float dayMix = glm::smoothstep(0.0f, 0.5f, elevation);          // warm -> white
        const glm::vec3 warmColor = glm::vec3(1.00f, 0.68f, 0.35f);
        const glm::vec3 dayColor  = glm::vec3(1.00f, 0.98f, 0.90f);
        const glm::vec3 sunHue    = glm::mix(warmColor, dayColor, dayMix);
        const float sunBright = cfg.DayBrightness * glm::smoothstep(0.0f, tw, elevation); // <= DayBrightness, 0 at night

        ctx.world.Each<SunMarker, LightningComponent>([&](EntityId sun) {
            ctx.world.Modify<LightningComponent>(sun, [&](auto& l) {
                if (l.Type != LightningType::Directional) return;
                l.Direction = glm::vec4(dir, 0.0f);
                l.Color = glm::vec4(sunHue * sunBright, 1.0f);
            });
        });

        // Ambient: small neutral day floor (ramped with the sun) + cool moon fill (ramped with night depth).
        const float dayA   = cfg.DayAmbient * glm::smoothstep(0.0f, tw, elevation);
        const float moonA  = cfg.MoonIntensity * glm::smoothstep(0.0f, tw, nightDepth);
        const glm::vec3 ambient = glm::vec3(dayA) + cfg.MoonColor * moonA;

        ctx.world.ModifySingleton<AtmosphereStateComponent>([&](AtmosphereStateComponent& a) {
            a.AmbientColor = glm::vec4(ambient, 1.0f);
        });
    }
```
Notes: the `dir` formula, `elevation`, and `Each<SunMarker, LightningComponent>`/`Modify<LightningComponent>` are unchanged from the existing system. `glm::smoothstep` and `glm::mix` are already available (the file uses GLM). If `ctx.world` does not expose `ModifySingleton` (it should — the ECS provides it and the system already uses `GetSingleton`/`Modify`), use `ctx.world.SetSingleton(AtmosphereStateComponent{ glm::vec4(ambient,1.0f) });` instead.

- [ ] **Step 2: Remove the stale reference comment if present**
The old `DayNightSystem` had a large commented-out reference block and `nightColor`/`brightness` locals; ensure none of the old local variables remain after the replacement (the new body fully replaces them). Keep the class declaration, `Name()`, and `Phase()` methods.

- [ ] **Step 3: Build the game**
`cmake --build out/build/msvc-win64-vs2026-enterprise --target game`
Expected: clean.

- [ ] **Step 4: Commit**
```bash
git add src/game/src/game.cpp
git commit -m "feat(daynight): capped/eased sun + cool varying moon ambient"
```

---

## Task 4: Editor Day/Night tuning panel

**Files:**
- Create: `src/editor/src/rendering/imgui/DayNightPanel.h`
- Create: `src/editor/src/rendering/imgui/DayNightPanel.cpp`
- Modify: `src/editor/CMakeLists.txt`
- Modify: wherever editor panels are drawn (the ImGui overlay/renderer that calls `DrawRenderStatsPanel`/`EcsInspectorPanel`) — add a `DrawDayNightPanel(...)` call.

Model the panel on `EcsInspectorPanel.cpp` (for `EditorContext` + the `ctx.App->ECSCommandRing.Push(...)` mechanism) and `RenderStatsPanel.cpp` (for the free-function shape). The panel context type is `EditorContext` (`src/editor/src/rendering/imgui/EditorContext.h`): it has `ApplicationContext* App`, `const ECS* World`, `std::shared_ptr<const ECS> WorldSnapshot`, etc.

- [ ] **Step 1: Create `DayNightPanel.h`**
```cpp
#pragma once
struct EditorContext;
void DrawDayNightPanel(const EditorContext& ctx, bool* open);
```

- [ ] **Step 2: Create `DayNightPanel.cpp`**
```cpp
#include "DayNightPanel.h"
#include "EditorContext.h"

#include <imgui.h>

#include "ECS.h"
#include "ECSCommands.h"
#include "ApplicationContext.h" // ctx.App->ECSCommandRing (same include EcsInspectorPanel uses)
#include "lib.h"                // SM_WARN

void DrawDayNightPanel(const EditorContext& ctx, bool* open)
{
    if (open && !*open) return;
    if (!ImGui::Begin("Day / Night", open)) { ImGui::End(); return; }

    const ECS* world = ctx.World;
    const DayNightConfigComponent* cur = world ? world->GetSingleton<DayNightConfigComponent>() : nullptr;
    if (!cur) { ImGui::TextDisabled("No DayNightConfig singleton"); ImGui::End(); return; }

    DayNightConfigComponent cfg = *cur; // edit a local copy
    bool changed = false;
    changed |= ImGui::SliderFloat("Cycle seconds", &cfg.CycleSeconds, 2.0f, 300.0f, "%.1f");
    changed |= ImGui::SliderFloat("Day brightness", &cfg.DayBrightness, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Moon intensity", &cfg.MoonIntensity, 0.0f, 1.0f, "%.3f");
    changed |= ImGui::SliderFloat("Twilight width", &cfg.TwilightWidth, 0.01f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Day ambient", &cfg.DayAmbient, 0.0f, 0.5f, "%.3f");
    changed |= ImGui::ColorEdit3("Moon color", &cfg.MoonColor.x);

    if (changed) {
        // A singleton is just a component on the hidden SingletonEntity(); edits go through the
        // command ring like any modify. Requires DayNightConfigComponent be registered in
        // ECSCommandProcessor (Task 1). AddComponent updates an existing component.
        ECSCommand cmd = ECSCommand::AddComponent(world->SingletonEntity(), cfg);
        if (!ctx.App->ECSCommandRing.Push(cmd)) {
            SM_WARN("DayNightPanel: ECSCommandRing full, edit dropped");
        }
    }
    ImGui::End();
}
```
Verify against `EcsInspectorPanel.cpp` that `ECSCommand::AddComponent(entity, value)` is the exact factory used for component modifies (it is — used there for Transform/Lightning/Material/etc.) and that `ctx.App->ECSCommandRing.Push(cmd)` matches. Confirm `ECS::SingletonEntity()` is accessible on `const ECS*` (it is a const accessor).

- [ ] **Step 3: Register the panel source in CMake**
In `src/editor/CMakeLists.txt`, next to the other imgui panel sources (e.g. `RenderStatsPanel.cpp`/`EcsInspectorPanel.cpp`), add:
```cmake
    src/rendering/imgui/DayNightPanel.cpp
```
(Match the exact relative path style used by the neighboring entries.)

- [ ] **Step 4: Call the panel from the overlay**
In `src/engine/.../ImGuiRenderer.cpp` (editor), the panels are drawn in `ImGuiRenderer::Render`. Note `DrawRenderStatsPanel(&s_ShowRenderStatsPanel)` is called in a region that only has the `world` local — but `DrawDayNightPanel` needs the full `EditorContext`. So add the call next to where the `EditorContext` (commonly `ctx`) is built and `m_EcsInspector.Draw(ctx)` is invoked. Add `#include "DayNightPanel.h"` near the other panel includes, then next to `m_EcsInspector.Draw(ctx);` add:
```cpp
        static bool s_ShowDayNightPanel = true;
        DrawDayNightPanel(ctx, &s_ShowDayNightPanel);
```
(Use the actual `EditorContext` variable name at that call site — read the lines around `m_EcsInspector.Draw(...)`.)

- [ ] **Step 5: Build the editor**
`cmake --build out/build/msvc-win64-vs2026-enterprise --target editor`
Expected: clean.

- [ ] **Step 6: Commit**
```bash
git add src/editor/src/rendering/imgui/DayNightPanel.h src/editor/src/rendering/imgui/DayNightPanel.cpp src/editor/CMakeLists.txt
git add -u src/editor
git commit -m "feat(daynight): editor Day/Night tuning panel"
```
(Stage only the panel files, the CMake change, and the overlay file you edited; verify with `git status`.)

---

## Task 5: Verification

**Files:** none (manual; fix-only if issues).

- [ ] **Step 1: Full rebuild + restart**
```
cmake --build out/build/msvc-win64-vs2026-enterprise --target editor
cmake --build out/build/msvc-win64-vs2026-enterprise --target game
```
Close any running editor; launch `out/build/msvc-win64-vs2026-enterprise/bin/Debug/editor.exe` fresh (ECS layout changed).

- [ ] **Step 2: Observe the cycle (default 60s)**
Confirm: daytime is clean with NO white blow-out; transitions are slow and smooth (no color snap at dawn/dusk); night is cool and visibly VARIES (brighter near deep-midnight, dimmer near dawn/dusk); fog and sun shadows still track the sun as before; the grid/outline/UI are unaffected.

- [ ] **Step 3: Exercise the Day/Night panel**
Open the "Day / Night" panel. Drag Cycle seconds, Day brightness, Moon intensity, Twilight width, Day ambient, and Moon color — confirm each changes the look live (within a frame or two; edits flow through the command ring → GameThread → DayNightSystem → AtmosphereStateComponent → lighting pass).

- [ ] **Step 4: Unit-test regression**
```
cmake --build out/build/msvc-win64-vs2026-enterprise --target test_ecs
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_ecs.exe
```
Expected: `All ECS tests passed.`

---

## Notes
- `ECS.h` changed → `ecs.dll` + `Engine` + `game` + `editor` rebuild and an editor **restart** are required (the running editor has the old component layout linked).
- The sun **direction** is intentionally unchanged so the fog (reads SunMarker light dir) and the shadow pass keep working. Only the sun color/brightness and the new ambient term changed.
- Moon is omnidirectional ambient (flat night lighting, no moon shadows) — the accepted tradeoff. A real moon directional with its own shadows is future work (would need multi-directional lighting).
