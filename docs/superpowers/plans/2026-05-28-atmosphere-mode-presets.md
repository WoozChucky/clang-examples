# Atmosphere Sky Mode + Presets Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Static sky mode (frozen sun, still casts shadows) and a preset dropdown to the Atmosphere panel, cutting the default visible knob count from ~21 to ~5 without removing tuning capability.

**Architecture:** The sky gradient, fog, and shadows all derive from the `SunMarker` directional light's direction, written each tick by `DayNightSystem`. Static mode just pins that direction at a fixed angle instead of animating it with `gameTime`; everything downstream is unchanged. Presets are an editor-only, hardcoded table that stamps concrete values into the Fog/Sky/DayNight singleton components via the existing command ring, so `world.json` keeps storing raw values.

**Tech Stack:** C++23, GLM (forced `[0,1]` depth, right-handed), nlohmann::json, NVRHI (DX12/VK), Dear ImGui, custom ECS (`ecs.dll`), hot-reloaded `Game.dll`.

**Spec:** `docs/superpowers/specs/2026-05-28-atmosphere-mode-presets-design.md`

---

## Build & Test Reference

- Configure/build (per CLAUDE.md, enterprise NOT installed — use community):
  - `cmake --preset msvc-win64-vs2026-community`
  - `cmake --build --preset msvc-win64-vs2026-community --target <target>`
- Test binaries land in `out/build/msvc-win64-vs2026-community/bin/Debug/`.
- **`ECS.h` changes** (Task 1) require rebuilding `ecs`, `editor`, and `game`, then **restarting the editor** — the running `editor.exe` has the old `DayNightConfigComponent` layout linked in.

---

## File Structure

| File | Responsibility | Task |
|------|----------------|------|
| `src/common/include/ECS.h` | `SkyMode` enum + 4 new fields on `DayNightConfigComponent` | 1 |
| `src/game/src/Atmosphere.h` (new) | Pure `SunDirectionFromAngles()` helper (header-only, glm-only) | 2 |
| `tests/test_atmosphere.cpp` (new) | Unit tests for the helper, serialization round-trip, preset match | 2,3,6 |
| `tests/CMakeLists.txt` | `test_atmosphere` target | 2 |
| `src/common/include/ComponentSerialization.h` | (de)serialize the 4 new fields, guarded reads | 3 |
| `src/game/src/game.cpp` | `DayNightSystem` mode branch | 4 |
| `src/engine/src/rendering/passes/SkyRenderPass.cpp` | Sun-disc/halo gate from `ShowSunDisc` | 5 |
| `src/editor/src/panels/AtmospherePresets.{h,cpp}` (new) | Preset table + `MatchPreset()` (imgui-free) | 6 |
| `src/editor/CMakeLists.txt` | Add `AtmospherePresets.cpp` to `editor` | 6 |
| `src/editor/src/panels/DayNightPanel.cpp` | Panel rewrite: mode + preset + Advanced section | 7 |

---

## Task 1: Data model — SkyMode enum + DayNightConfig fields

**Files:**
- Modify: `src/common/include/ECS.h:135-142`

- [ ] **Step 1: Add the `SkyMode` enum and four fields**

In `src/common/include/ECS.h`, replace the existing `DayNightConfigComponent` (lines 135-142):

```cpp
// Selects how the sky/sun behave. DynamicCycle animates the sun over time;
// Static freezes it at a fixed angle (still lit, still casts shadows).
enum class SkyMode : int { DynamicCycle = 0, Static = 1 };

struct DayNightConfigComponent {
    float     CycleSeconds  = 60.0f;                 // full day length (was 10)
    float     DayBrightness = 1.0f;                  // peak sun brightness (cap <= 1.0, no blow-out)
    float     MoonIntensity = 0.15f;                 // night ambient strength
    float     TwilightWidth = 0.25f;                 // smoothstep band for dawn/dusk easing (elevation units)
    float     DayAmbient    = 0.08f;                 // neutral daytime ambient floor
    glm::vec3 MoonColor     = glm::vec3(0.10f, 0.14f, 0.26f); // cool blue night fill

    // --- Sky mode (added 2026-05-28) ---
    SkyMode   Mode                = SkyMode::DynamicCycle; // default preserves the animated cycle
    float     StaticSunElevDeg    = 50.0f;           // 0 = horizon, 90 = overhead (Static only)
    float     StaticSunAzimuthDeg = 30.0f;           // compass angle around +Y (Static only)
    bool      ShowSunDisc         = true;            // draw the sun disc/halo (mainly for Static)
};
```

- [ ] **Step 2: Verify it compiles (build ecs)**

Run: `cmake --build --preset msvc-win64-vs2026-community --target ecs`
Expected: builds clean. The struct stays trivially-copyable (enum is `int`-backed), so it still crosses the Seqlock/command-ring path.

- [ ] **Step 3: Commit**

```bash
git add src/common/include/ECS.h
git commit -m "feat(ecs): SkyMode enum + static-sun fields on DayNightConfigComponent"
```

---

## Task 2: Pure helper `SunDirectionFromAngles` + test target

**Files:**
- Create: `src/game/src/Atmosphere.h`
- Create: `tests/test_atmosphere.cpp`
- Modify: `tests/CMakeLists.txt` (append a new target after `test_followcam`)

- [ ] **Step 1: Write the failing test**

Create `tests/test_atmosphere.cpp`:

```cpp
#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>

#include "Atmosphere.h" // SunDirectionFromAngles

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

// Engine convention: sun light direction points FROM the sun toward the scene,
// so an overhead noon sun points straight down (dir.y == -1).
static void T00_overhead_points_down()
{
    const glm::vec3 d = SunDirectionFromAngles(90.0f, 0.0f);
    EXPECT(near(d.x, 0.0f));
    EXPECT(near(d.y, -1.0f));
    EXPECT(near(d.z, 0.0f));
}

// At the horizon (elevation 0) the vertical component is zero.
static void T01_horizon_is_flat()
{
    const glm::vec3 d = SunDirectionFromAngles(0.0f, 0.0f);
    EXPECT(near(d.y, 0.0f));
    EXPECT(near(glm::length(d), 1.0f)); // always unit length
}

// Azimuth rotates the horizontal component around +Y.
static void T02_azimuth_rotates_horizontal()
{
    const glm::vec3 d = SunDirectionFromAngles(0.0f, 90.0f);
    EXPECT(near(d.x, 1.0f)); // sin(90deg) on x
    EXPECT(near(d.z, 0.0f)); // cos(90deg) on z
}

// Elevation is clamped to [0,90]: out-of-range stays unit and finite.
static void T03_elevation_clamped()
{
    const glm::vec3 d = SunDirectionFromAngles(140.0f, 0.0f);
    EXPECT(near(glm::length(d), 1.0f));
    EXPECT(near(d.y, -1.0f)); // clamped to 90 -> straight down
}

int main()
{
    T00_overhead_points_down();
    T01_horizon_is_flat();
    T02_azimuth_rotates_horizontal();
    T03_elevation_clamped();
    if (g_Failures == 0) { std::printf("All atmosphere tests passed.\n"); return 0; }
    std::fprintf(stderr, "%d atmosphere test(s) failed.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 2: Add the `test_atmosphere` target**

Append to `tests/CMakeLists.txt` (mirrors the `test_followcam` block):

```cmake
add_executable(test_atmosphere
    test_atmosphere.cpp
)

target_link_libraries(test_atmosphere PRIVATE
    glm::glm
)

target_include_directories(test_atmosphere PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/game/src
)

target_compile_definitions(test_atmosphere PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_atmosphere PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 3: Run test to verify it fails**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_atmosphere
```
Expected: FAIL to compile — `Atmosphere.h` not found / `SunDirectionFromAngles` undefined.

- [ ] **Step 4: Write the helper**

Create `src/game/src/Atmosphere.h`:

```cpp
#pragma once

#include <cmath> // std::sin, std::cos

#include <glm/glm.hpp> // glm::vec3, glm::radians, glm::clamp, glm::normalize

// Sun light direction for a FIXED sun angle, matching the engine convention used by
// DayNightSystem's animated path: the vector points FROM the sun toward the scene, so an
// overhead (noon) sun points straight down (y == -1) and a horizon sun has y == 0.
//
//   elevDeg    : 0 = on the horizon, 90 = directly overhead. Clamped to [0, 90].
//   azimuthDeg : compass rotation of the horizontal component around the +Y axis.
//
// Returns a unit vector.
inline glm::vec3 SunDirectionFromAngles(float elevDeg, float azimuthDeg)
{
    const float E = glm::radians(glm::clamp(elevDeg, 0.0f, 90.0f));
    const float A = glm::radians(azimuthDeg);
    return glm::normalize(glm::vec3(
        std::cos(E) * std::sin(A),
        -std::sin(E),
        std::cos(E) * std::cos(A)));
}
```

- [ ] **Step 5: Run test to verify it passes**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_atmosphere
./out/build/msvc-win64-vs2026-community/bin/Debug/test_atmosphere.exe
```
Expected: `All atmosphere tests passed.`

- [ ] **Step 6: Commit**

```bash
git add src/game/src/Atmosphere.h tests/test_atmosphere.cpp tests/CMakeLists.txt
git commit -m "feat(game): SunDirectionFromAngles helper + test_atmosphere target"
```

---

## Task 3: Serialize the new fields (backward-compatible)

**Files:**
- Modify: `src/common/include/ComponentSerialization.h:276-292`
- Modify: `tests/test_atmosphere.cpp` (add round-trip tests)
- Modify: `tests/CMakeLists.txt` (`test_atmosphere` now needs ecs + nlohmann + engine include for ComponentSerialization.h)

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_atmosphere.cpp` — add the include near the top (after the existing includes):

```cpp
#include <nlohmann/json.hpp>
#include "ECS.h"
#include "ComponentSerialization.h"
```

Add these test functions before `main()`:

```cpp
// New fields survive a to_json -> from_json round-trip.
static void T10_daynight_roundtrip()
{
    DayNightConfigComponent in{};
    in.Mode                = SkyMode::Static;
    in.StaticSunElevDeg    = 12.5f;
    in.StaticSunAzimuthDeg = 200.0f;
    in.ShowSunDisc         = false;

    const nlohmann::json j = in;          // to_json
    DayNightConfigComponent out = j.get<DayNightConfigComponent>(); // from_json

    EXPECT(out.Mode == SkyMode::Static);
    EXPECT(near(out.StaticSunElevDeg, 12.5f));
    EXPECT(near(out.StaticSunAzimuthDeg, 200.0f));
    EXPECT(out.ShowSunDisc == false);
}

// Old world.json (new keys absent) loads with the new fields defaulted to DynamicCycle.
static void T11_daynight_missing_keys_default()
{
    // Only the original six keys present (an old file).
    nlohmann::json j = {
        {"CycleSeconds", 60.0f}, {"DayBrightness", 1.0f}, {"MoonIntensity", 0.15f},
        {"TwilightWidth", 0.25f}, {"DayAmbient", 0.08f},
        {"MoonColor", nlohmann::json::array({0.1f, 0.14f, 0.26f})}
    };
    DayNightConfigComponent out = j.get<DayNightConfigComponent>();
    EXPECT(out.Mode == SkyMode::DynamicCycle);
    EXPECT(out.ShowSunDisc == true);
    EXPECT(near(out.StaticSunElevDeg, 50.0f)); // struct default
}
```

Register them in `main()` (before the pass/fail print):

```cpp
    T10_daynight_roundtrip();
    T11_daynight_missing_keys_default();
```

- [ ] **Step 2: Update the `test_atmosphere` target deps**

In `tests/CMakeLists.txt`, replace the `test_atmosphere` `target_link_libraries` and `target_include_directories` blocks with:

```cmake
target_link_libraries(test_atmosphere PRIVATE
    glm::glm
    ecs
    nlohmann_json::nlohmann_json
)

target_include_directories(test_atmosphere PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/game/src
)
```

- [ ] **Step 3: Run test to verify it fails**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_atmosphere
./out/build/msvc-win64-vs2026-community/bin/Debug/test_atmosphere.exe
```
Expected: FAIL — `T10` reads back `Mode == DynamicCycle` (serializer drops the new fields) and/or `T11` throws in `from_json` (unguarded `j.at` is fine here, but the new fields aren't read).

- [ ] **Step 4: Update the serializers**

In `src/common/include/ComponentSerialization.h`, replace `to_json`/`from_json` for `DayNightConfigComponent` (lines 276-292):

```cpp
inline void to_json(nlohmann::json& j, const DayNightConfigComponent& t) {
    j = nlohmann::json{
        {"CycleSeconds", t.CycleSeconds},
        {"DayBrightness", t.DayBrightness},
        {"MoonIntensity", t.MoonIntensity},
        {"TwilightWidth", t.TwilightWidth},
        {"DayAmbient", t.DayAmbient},
        {"MoonColor", t.MoonColor},
        {"Mode", static_cast<int>(t.Mode)},
        {"StaticSunElevDeg", t.StaticSunElevDeg},
        {"StaticSunAzimuthDeg", t.StaticSunAzimuthDeg},
        {"ShowSunDisc", t.ShowSunDisc}};
}
inline void from_json(const nlohmann::json& j, DayNightConfigComponent& t) {
    j.at("CycleSeconds").get_to(t.CycleSeconds);
    j.at("DayBrightness").get_to(t.DayBrightness);
    j.at("MoonIntensity").get_to(t.MoonIntensity);
    j.at("TwilightWidth").get_to(t.TwilightWidth);
    j.at("DayAmbient").get_to(t.DayAmbient);
    j.at("MoonColor").get_to(t.MoonColor);
    // New fields are optional so existing world.json files still load (default = DynamicCycle).
    if (j.contains("Mode"))                t.Mode = static_cast<SkyMode>(j.at("Mode").get<int>());
    if (j.contains("StaticSunElevDeg"))    j.at("StaticSunElevDeg").get_to(t.StaticSunElevDeg);
    if (j.contains("StaticSunAzimuthDeg")) j.at("StaticSunAzimuthDeg").get_to(t.StaticSunAzimuthDeg);
    if (j.contains("ShowSunDisc"))         j.at("ShowSunDisc").get_to(t.ShowSunDisc);
}
```

- [ ] **Step 5: Run test to verify it passes**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_atmosphere
./out/build/msvc-win64-vs2026-community/bin/Debug/test_atmosphere.exe
```
Expected: `All atmosphere tests passed.`

- [ ] **Step 6: Commit**

```bash
git add src/common/include/ComponentSerialization.h tests/test_atmosphere.cpp tests/CMakeLists.txt
git commit -m "feat(serialization): persist SkyMode fields, optional on load"
```

---

## Task 4: DayNightSystem mode branch

**Files:**
- Modify: `src/game/src/game.cpp:1-20` (add include) and `:56-66` (sun-dir branch)

- [ ] **Step 1: Add the Atmosphere.h include**

In `src/game/src/game.cpp`, after the existing local includes (near line 9, after `#include "Actions.h"`), add:

```cpp
#include "Atmosphere.h" // SunDirectionFromAngles (static-mode sun direction)
```

- [ ] **Step 2: Branch the sun-direction computation on Mode**

In `DayNightSystem::Update` (`src/game/src/game.cpp`), replace the cycle/dir computation (the lines computing `cycle`, `gameTime`, `phase`, `theta`, and `dir` — currently lines 61-65):

```cpp
        // Sun direction: animated by the cycle (DynamicCycle) or pinned at a fixed angle (Static).
        glm::vec3 dir;
        if (cfg.Mode == SkyMode::Static) {
            dir = SunDirectionFromAngles(cfg.StaticSunElevDeg, cfg.StaticSunAzimuthDeg);
        } else {
            const float cycle = glm::max(cfg.CycleSeconds, 0.001f);
            const double gameTime = ctx.gameTime;
            const auto phase = static_cast<float>(std::fmod(gameTime, static_cast<double>(cycle)) / static_cast<double>(cycle));
            const float theta = phase * 6.28318530718f;
            dir = glm::normalize(glm::vec3(0.0f, -cosf(theta), sinf(theta)));
        }
```

Everything after this point (the `elevation` / `nightDepth` / `sunHue` / `sunBright` / ambient math and the light + `AtmosphereStateComponent` writes) is unchanged — it already operates purely on `dir`.

- [ ] **Step 3: Build game (and ecs/editor if not already from Task 1)**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target game
```
Expected: builds clean.

- [ ] **Step 4: Manual smoke (deferred to Task 7 full verify)**

No automated test for the system (it writes into live ECS singletons). The `SunDirectionFromAngles` math is covered by Task 2. Full manual verification happens once the panel exists (Task 7). For now confirm the editor still runs and the dynamic cycle is unchanged:
- Launch the editor, observe the sun still animates as before (Mode defaults to DynamicCycle).

- [ ] **Step 5: Commit**

```bash
git add src/game/src/game.cpp
git commit -m "feat(game): DayNightSystem freezes sun in Static mode"
```

---

## Task 5: Sun-disc/halo gate in SkyRenderPass

**Files:**
- Modify: `src/engine/src/rendering/passes/SkyRenderPass.cpp:115-118` and `:166`

**Why zero the color, not the disc thresholds:** the sky pixel shader adds a sun *disc* (`smoothstep(uDisc.x, uDisc.y, sdot)`) **and** a *halo* (`pow(saturate(sdot), uSunColor.w) * 0.5`). The halo ignores `uDisc`, so raising the disc thresholds alone would leave a visible glow. Zeroing `uSunColor.rgb` removes both (`sky += 0 * (disc + halo)`), and leaves the gradient and moon untouched.

- [ ] **Step 1: Read the `ShowSunDisc` flag alongside the SkyComponent**

In `SkyRenderPass::Render`, just after the `SkyComponent s{}` read block (currently lines 115-118), add:

```cpp
    bool showSunDisc = true;
    if (world) {
        if (const auto* dn = world->GetSingleton<DayNightConfigComponent>())
            showSunDisc = dn->ShowSunDisc;
    }
```

- [ ] **Step 2: Gate the sun color when the disc is hidden**

Replace the sun-color CB assignment (currently line 166):

```cpp
    cb.SunColor     = glm::vec4(s.SunColor,  s.SunGlow);
```

with:

```cpp
    // ShowSunDisc == false zeroes the sun color so both the disc and its halo vanish
    // (the halo term in the sky shader is independent of the uDisc thresholds).
    cb.SunColor     = glm::vec4(showSunDisc ? s.SunColor : glm::vec3(0.0f), s.SunGlow);
```

- [ ] **Step 3: Build engine**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target Engine
```
Expected: builds clean. (`DayNightConfigComponent` is already visible via `ECS.h`, included by the pass.)

- [ ] **Step 4: Manual verification (deferred to Task 7)**

Full toggle test happens with the panel in Task 7. Behavior to confirm later: unticking "Show sun disc" removes the sun disc and its glow, leaving the gradient and moon intact.

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/rendering/passes/SkyRenderPass.cpp
git commit -m "feat(render): hide sun disc + halo when ShowSunDisc is off"
```

---

## Task 6: Atmosphere preset table + MatchPreset

**Files:**
- Create: `src/editor/src/panels/AtmospherePresets.h`
- Create: `src/editor/src/panels/AtmospherePresets.cpp`
- Modify: `src/editor/CMakeLists.txt` (add the new .cpp to the `editor` target)
- Modify: `tests/test_atmosphere.cpp` (add match tests) and `tests/CMakeLists.txt` (compile the preset .cpp into `test_atmosphere`)

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_atmosphere.cpp` — add the include with the others near the top:

```cpp
#include "AtmospherePresets.h"
```

Add these tests before `main()`:

```cpp
// A preset's own values match itself.
static void T20_preset_matches_itself()
{
    const AtmospherePreset& p = kAtmospherePresets[0];
    const char* name = MatchPreset(p.Fog, p.Sky, p.DayNight);
    EXPECT(name != nullptr);
    EXPECT(std::string(name) == p.Name);
}

// Mode/static-angle/cycle differences do NOT break a palette match: only the
// palette fields (fog + sky colors/densities, day/night tunables) are compared.
static void T21_match_ignores_mode_and_static_angle()
{
    AtmospherePreset p = kAtmospherePresets[0];
    DayNightConfigComponent dn = p.DayNight;
    dn.Mode             = SkyMode::Static;  // mode differs
    dn.StaticSunElevDeg = 5.0f;             // static angle differs
    const char* name = MatchPreset(p.Fog, p.Sky, dn);
    EXPECT(name != nullptr);
    EXPECT(std::string(name) == p.Name);
}

// Edited palette values match no preset -> "Custom" (nullptr).
static void T22_edited_values_are_custom()
{
    AtmospherePreset p = kAtmospherePresets[0];
    SkyComponent sky = p.Sky;
    sky.DayZenith = glm::vec3(0.123f, 0.456f, 0.789f); // not any preset's value
    const char* name = MatchPreset(p.Fog, sky, p.DayNight);
    EXPECT(name == nullptr);
}
```

Register in `main()`:

```cpp
    T20_preset_matches_itself();
    T21_match_ignores_mode_and_static_angle();
    T22_edited_values_are_custom();
```

Also add `#include <string>` near the top of the test file (for `std::string` comparisons).

- [ ] **Step 2: Compile the preset .cpp into the test target**

In `tests/CMakeLists.txt`, update the `test_atmosphere` `add_executable` and includes:

```cmake
add_executable(test_atmosphere
    test_atmosphere.cpp
    ${CMAKE_SOURCE_DIR}/src/editor/src/panels/AtmospherePresets.cpp
)
```

and add the panels dir to its include list:

```cmake
target_include_directories(test_atmosphere PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/game/src
    ${CMAKE_SOURCE_DIR}/src/editor/src/panels
)
```

- [ ] **Step 3: Run test to verify it fails**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_atmosphere
```
Expected: FAIL to compile — `AtmospherePresets.h` not found.

- [ ] **Step 4: Write the preset header**

Create `src/editor/src/panels/AtmospherePresets.h`:

```cpp
#pragma once

#include <cstddef> // std::size_t

#include "ECS.h" // FogComponent, SkyComponent, DayNightConfigComponent

// A named bundle of atmosphere palette values. Selecting a preset in the Atmosphere panel
// stamps these into the live Fog/Sky/DayNight singleton components (stamp-and-forget): the
// values then serialize into world.json as raw data and no preset name is persisted.
//
// The DayNight payload carries only palette/cycle tunables; the panel keeps the user's
// current Mode / static-sun angle when applying a preset (see MatchPreset, which ignores
// those fields).
struct AtmospherePreset {
    const char*             Name;
    FogComponent            Fog;
    SkyComponent            Sky;
    DayNightConfigComponent DayNight;
};

// The hardcoded preset list and its length.
extern const AtmospherePreset kAtmospherePresets[];
extern const std::size_t      kAtmospherePresetCount;

// Returns the name of the preset whose palette matches the given components (field-by-field
// with a small float epsilon), or nullptr if none match ("Custom"). Mode, StaticSunElevDeg,
// StaticSunAzimuthDeg, and ShowSunDisc are intentionally ignored — they are mode controls,
// not palette.
const char* MatchPreset(const FogComponent& fog,
                        const SkyComponent& sky,
                        const DayNightConfigComponent& dayNight);
```

- [ ] **Step 5: Write the preset table + match logic**

Create `src/editor/src/panels/AtmospherePresets.cpp`:

```cpp
#include "AtmospherePresets.h"

#include <cmath> // std::fabs

namespace {
    constexpr float kEps = 1e-4f;

    bool feq(float a, float b)              { return std::fabs(a - b) <= kEps; }
    bool veq(const glm::vec3& a, const glm::vec3& b) {
        return feq(a.x, b.x) && feq(a.y, b.y) && feq(a.z, b.z);
    }

    bool FogEq(const FogComponent& a, const FogComponent& b) {
        return a.Enabled == b.Enabled
            && feq(a.DayDensity, b.DayDensity) && feq(a.NightDensity, b.NightDensity)
            && veq(a.DayColor, b.DayColor) && veq(a.NightColor, b.NightColor);
    }
    bool SkyEq(const SkyComponent& a, const SkyComponent& b) {
        return a.Enabled == b.Enabled
            && veq(a.DayZenith, b.DayZenith)   && veq(a.DayHorizon, b.DayHorizon)
            && veq(a.NightZenith, b.NightZenith) && veq(a.NightHorizon, b.NightHorizon)
            && veq(a.SunColor, b.SunColor)     && feq(a.SunRadiusDeg, b.SunRadiusDeg)
            && feq(a.SunGlow, b.SunGlow)
            && veq(a.MoonColor, b.MoonColor)   && feq(a.MoonRadiusDeg, b.MoonRadiusDeg)
            && feq(a.MoonGlow, b.MoonGlow);
    }
    // Palette/cycle tunables only — Mode and static-sun fields are deliberately excluded.
    bool DayNightPaletteEq(const DayNightConfigComponent& a, const DayNightConfigComponent& b) {
        return feq(a.CycleSeconds, b.CycleSeconds) && feq(a.DayBrightness, b.DayBrightness)
            && feq(a.MoonIntensity, b.MoonIntensity) && feq(a.TwilightWidth, b.TwilightWidth)
            && feq(a.DayAmbient, b.DayAmbient) && veq(a.MoonColor, b.MoonColor);
    }
}

// Preset palettes. The first field of each DayNight payload mirrors the struct defaults;
// only values that define the "look" are varied per preset. Mode is left at the default
// (DynamicCycle) and ignored by MatchPreset.
const AtmospherePreset kAtmospherePresets[] = {
    {
        "Clear Day",
        /*Fog*/ { true, 0.0f, 0.06f, glm::vec3(0.60f, 0.70f, 0.80f), glm::vec3(0.03f, 0.04f, 0.08f) },
        /*Sky*/ { true,
                  glm::vec3(0.20f, 0.40f, 0.85f), glm::vec3(0.70f, 0.80f, 0.95f),
                  glm::vec3(0.01f, 0.02f, 0.06f), glm::vec3(0.04f, 0.05f, 0.12f),
                  glm::vec3(1.00f, 0.95f, 0.80f), 3.0f, 64.0f,
                  glm::vec3(0.80f, 0.85f, 1.00f), 2.5f, 128.0f },
        /*DayNight*/ { 60.0f, 1.0f, 0.15f, 0.25f, 0.08f, glm::vec3(0.10f, 0.14f, 0.26f) }
    },
    {
        "Overcast",
        /*Fog*/ { true, 0.015f, 0.10f, glm::vec3(0.72f, 0.74f, 0.78f), glm::vec3(0.10f, 0.11f, 0.14f) },
        /*Sky*/ { true,
                  glm::vec3(0.55f, 0.58f, 0.62f), glm::vec3(0.78f, 0.80f, 0.84f),
                  glm::vec3(0.05f, 0.06f, 0.08f), glm::vec3(0.10f, 0.11f, 0.13f),
                  glm::vec3(0.90f, 0.90f, 0.88f), 3.0f, 32.0f,
                  glm::vec3(0.70f, 0.74f, 0.82f), 2.5f, 96.0f },
        /*DayNight*/ { 60.0f, 0.7f, 0.12f, 0.30f, 0.12f, glm::vec3(0.12f, 0.13f, 0.18f) }
    },
    {
        "Sunset",
        /*Fog*/ { true, 0.02f, 0.09f, glm::vec3(0.85f, 0.55f, 0.40f), glm::vec3(0.05f, 0.04f, 0.10f) },
        /*Sky*/ { true,
                  glm::vec3(0.25f, 0.30f, 0.65f), glm::vec3(0.95f, 0.55f, 0.30f),
                  glm::vec3(0.02f, 0.02f, 0.07f), glm::vec3(0.10f, 0.05f, 0.10f),
                  glm::vec3(1.00f, 0.65f, 0.35f), 4.0f, 48.0f,
                  glm::vec3(0.80f, 0.82f, 1.00f), 2.5f, 128.0f },
        /*DayNight*/ { 60.0f, 0.9f, 0.15f, 0.35f, 0.07f, glm::vec3(0.12f, 0.10f, 0.22f) }
    },
    {
        "Night",
        /*Fog*/ { true, 0.05f, 0.14f, glm::vec3(0.10f, 0.12f, 0.20f), glm::vec3(0.02f, 0.03f, 0.07f) },
        /*Sky*/ { true,
                  glm::vec3(0.02f, 0.03f, 0.10f), glm::vec3(0.05f, 0.07f, 0.16f),
                  glm::vec3(0.01f, 0.01f, 0.04f), glm::vec3(0.02f, 0.03f, 0.08f),
                  glm::vec3(1.00f, 0.95f, 0.80f), 3.0f, 64.0f,
                  glm::vec3(0.85f, 0.88f, 1.00f), 3.0f, 96.0f },
        /*DayNight*/ { 60.0f, 0.4f, 0.30f, 0.25f, 0.04f, glm::vec3(0.10f, 0.14f, 0.30f) }
    },
};
const std::size_t kAtmospherePresetCount = sizeof(kAtmospherePresets) / sizeof(kAtmospherePresets[0]);

const char* MatchPreset(const FogComponent& fog,
                        const SkyComponent& sky,
                        const DayNightConfigComponent& dayNight)
{
    for (std::size_t i = 0; i < kAtmospherePresetCount; ++i) {
        const AtmospherePreset& p = kAtmospherePresets[i];
        if (FogEq(fog, p.Fog) && SkyEq(sky, p.Sky) && DayNightPaletteEq(dayNight, p.DayNight))
            return p.Name;
    }
    return nullptr;
}
```

- [ ] **Step 6: Add the .cpp to the editor target**

In `src/editor/CMakeLists.txt`, the `editor` target lists panel sources with paths relative to that file (e.g. `src/panels/DayNightPanel.cpp` at line 17). Add, right after that line:

```cmake
    src/panels/AtmospherePresets.cpp
```

- [ ] **Step 7: Run test to verify it passes**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_atmosphere
./out/build/msvc-win64-vs2026-community/bin/Debug/test_atmosphere.exe
```
Expected: `All atmosphere tests passed.`

- [ ] **Step 8: Commit**

```bash
git add src/editor/src/panels/AtmospherePresets.h src/editor/src/panels/AtmospherePresets.cpp src/editor/CMakeLists.txt tests/test_atmosphere.cpp tests/CMakeLists.txt
git commit -m "feat(editor): atmosphere preset table + MatchPreset with tests"
```

---

## Task 7: Atmosphere panel rewrite

**Files:**
- Modify: `src/editor/src/panels/DayNightPanel.cpp` (full rewrite of `DrawDayNightPanel`)

- [ ] **Step 1: Rewrite the panel**

Replace the entire body of `DrawDayNightPanel` in `src/editor/src/panels/DayNightPanel.cpp`. Keep the existing `#include`s and the `PushSingletonEdit` helper; add `#include "AtmospherePresets.h"` to the include block and `#include <cstring>` for `std::strcmp`. New function body:

```cpp
void DrawDayNightPanel(const EditorContext& ctx, bool* open)
{
    if (open && !*open) return;
    if (!ImGui::Begin("Atmosphere", open)) { ImGui::End(); return; }

    const ECS* world = ctx.World;
    if (!world) { ImGui::TextDisabled("No world"); ImGui::End(); return; }

    const DayNightConfigComponent* dnCur = world->GetSingleton<DayNightConfigComponent>();
    const FogComponent*            fogCur = world->GetSingleton<FogComponent>();
    const SkyComponent*            skyCur = world->GetSingleton<SkyComponent>();
    if (!dnCur) { ImGui::TextDisabled("No DayNightConfig singleton"); ImGui::End(); return; }

    DayNightConfigComponent cfg = *dnCur;

    // --- Mode ---
    {
        int mode = static_cast<int>(cfg.Mode);
        const char* modeNames[] = { "Dynamic Cycle", "Static" };
        if (ImGui::Combo("Mode", &mode, modeNames, IM_ARRAYSIZE(modeNames))) {
            cfg.Mode = static_cast<SkyMode>(mode);
            PushSingletonEdit(ctx, world, cfg, "mode");
        }
    }

    // --- Preset ---
    if (fogCur && skyCur) {
        const char* active = MatchPreset(*fogCur, *skyCur, *dnCur); // nullptr -> Custom
        const char* label  = active ? active : "Custom";
        if (ImGui::BeginCombo("Preset", label)) {
            for (std::size_t i = 0; i < kAtmospherePresetCount; ++i) {
                const AtmospherePreset& p = kAtmospherePresets[i];
                const bool selected = active && std::strcmp(active, p.Name) == 0;
                if (ImGui::Selectable(p.Name, selected)) {
                    // Stamp palette into all three singletons. Preserve the user's current
                    // Mode + static-sun angle (presets carry only palette/cycle values).
                    DayNightConfigComponent dn = p.DayNight;
                    dn.Mode                = cfg.Mode;
                    dn.StaticSunElevDeg    = cfg.StaticSunElevDeg;
                    dn.StaticSunAzimuthDeg = cfg.StaticSunAzimuthDeg;
                    dn.ShowSunDisc         = cfg.ShowSunDisc;
                    PushSingletonEdit(ctx, world, p.Fog, "preset fog");
                    PushSingletonEdit(ctx, world, p.Sky, "preset sky");
                    PushSingletonEdit(ctx, world, dn,    "preset day/night");
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();

    // --- Mode-specific common controls ---
    bool dnChanged = false;
    if (cfg.Mode == SkyMode::DynamicCycle) {
        dnChanged |= ImGui::SliderFloat("Cycle seconds", &cfg.CycleSeconds, 2.0f, 300.0f, "%.1f");
        dnChanged |= ImGui::SliderFloat("Day brightness", &cfg.DayBrightness, 0.0f, 1.0f, "%.2f");
        dnChanged |= ImGui::SliderFloat("Moon intensity", &cfg.MoonIntensity, 0.0f, 1.0f, "%.3f");
    } else {
        dnChanged |= ImGui::SliderFloat("Sun elevation", &cfg.StaticSunElevDeg, 0.0f, 90.0f, "%.1f deg");
        dnChanged |= ImGui::SliderFloat("Sun azimuth",   &cfg.StaticSunAzimuthDeg, 0.0f, 360.0f, "%.1f deg");
        dnChanged |= ImGui::SliderFloat("Day brightness", &cfg.DayBrightness, 0.0f, 1.0f, "%.2f");
        dnChanged |= ImGui::Checkbox("Show sun disc", &cfg.ShowSunDisc);
    }

    // --- Advanced (collapsed) ---
    if (ImGui::CollapsingHeader("Advanced")) {
        ImGui::SeparatorText("Day / Night");
        dnChanged |= ImGui::SliderFloat("Twilight width", &cfg.TwilightWidth, 0.01f, 1.0f, "%.2f");
        dnChanged |= ImGui::SliderFloat("Day ambient",    &cfg.DayAmbient, 0.0f, 0.5f, "%.3f");
        dnChanged |= ImGui::ColorEdit3("Moon color (fill)", &cfg.MoonColor.x);

        if (skyCur) {
            ImGui::SeparatorText("Sky");
            SkyComponent sky = *skyCur;
            bool sChanged = false;
            sChanged |= ImGui::Checkbox("Sky enabled", &sky.Enabled);
            sChanged |= ImGui::ColorEdit3("Day zenith",    &sky.DayZenith.x);
            sChanged |= ImGui::ColorEdit3("Day horizon",   &sky.DayHorizon.x);
            sChanged |= ImGui::ColorEdit3("Night zenith",  &sky.NightZenith.x);
            sChanged |= ImGui::ColorEdit3("Night horizon", &sky.NightHorizon.x);
            sChanged |= ImGui::ColorEdit3("Sun color",     &sky.SunColor.x);
            sChanged |= ImGui::SliderFloat("Sun radius (deg)",  &sky.SunRadiusDeg, 0.5f, 15.0f, "%.1f");
            sChanged |= ImGui::SliderFloat("Sun glow",          &sky.SunGlow, 1.0f, 512.0f, "%.0f");
            sChanged |= ImGui::ColorEdit3("Moon color",    &sky.MoonColor.x);
            sChanged |= ImGui::SliderFloat("Moon radius (deg)", &sky.MoonRadiusDeg, 0.5f, 15.0f, "%.1f");
            sChanged |= ImGui::SliderFloat("Moon glow",         &sky.MoonGlow, 1.0f, 512.0f, "%.0f");
            if (sChanged) PushSingletonEdit(ctx, world, sky, "sky");
        }

        if (fogCur) {
            ImGui::SeparatorText("Fog");
            FogComponent fog = *fogCur;
            bool fChanged = false;
            fChanged |= ImGui::Checkbox("Fog enabled", &fog.Enabled);
            fChanged |= ImGui::SliderFloat("Day density",   &fog.DayDensity,   0.0f, 0.05f, "%.4f");
            fChanged |= ImGui::SliderFloat("Night density", &fog.NightDensity, 0.0f, 0.30f, "%.3f");
            fChanged |= ImGui::ColorEdit3("Day color",   &fog.DayColor.x);
            fChanged |= ImGui::ColorEdit3("Night color", &fog.NightColor.x);
            if (fChanged) PushSingletonEdit(ctx, world, fog, "fog");
        }
    }

    if (dnChanged) PushSingletonEdit(ctx, world, cfg, "day/night");

    ImGui::End();
}
```

- [ ] **Step 2: Build editor + game and restart**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target ecs
cmake --build --preset msvc-win64-vs2026-community --target game
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: all build clean. Restart the editor (ECS layout changed back in Task 1).

- [ ] **Step 3: Manual verification (the full feature)**

Launch the editor and open the **Atmosphere** panel. Confirm:
1. **Default view** shows ~5 controls: Mode, Preset, Cycle seconds, Day brightness, Moon intensity, plus a collapsed **Advanced** header.
2. **Mode = Static** swaps the common controls to Sun elevation / Sun azimuth / Day brightness / Show sun disc; the sun stops moving and freezes at the set angle.
3. **Shadows still render** in Static mode (move the sun elevation/azimuth and watch shadow direction change).
4. **Show sun disc** off removes the sun disc *and* its glow; gradient + moon remain.
5. **Presets** (Clear Day / Overcast / Sunset / Night) visibly change the look; the dropdown shows the preset name, and flips to **Custom** after you edit any palette knob under Advanced.
6. **Persistence:** save the scene (File menu), reload, and confirm Mode + static angle + palette survive in `world.json` (check the `"Environment"` block has the new keys).

- [ ] **Step 4: Run the full unit-test sweep**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_atmosphere
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_atmosphere.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `All atmosphere tests passed.` and `All ECS tests passed.`

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/panels/DayNightPanel.cpp
git commit -m "feat(editor): Atmosphere panel mode switch + presets + Advanced section"
```

---

## Self-Review Notes (spec coverage)

- Goal 1 (cut knob count) → Task 7 (default ~5, Advanced collapse).
- Goal 2 (Static mode, lit + shadows) → Tasks 1, 2, 4 (+ verified Task 7 step 3.3).
- Goal 3 (presets) → Tasks 6, 7.
- Spec §3 sun-disc gate → Task 5 (corrected to zero sun color so the halo is also removed).
- Spec §6 backward-compatible serialization → Task 3 (guarded reads + missing-key test).
- Spec testing section: (a) sun-dir helper → Task 2; (b) preset→Custom match → Task 6; serialization round-trip → Task 3.
```
