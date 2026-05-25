# Persist Scene Atmosphere in world.json — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist per-scene atmosphere (Fog + Sky + DayNight config) in `world.json` by migrating Fog/Sky to singleton ECS components alongside `DayNightConfigComponent`, making the ECS snapshot the single source of truth and consolidating editing into one "Atmosphere" panel.

**Architecture:** Fog/Sky become singleton ECS components in `ECS.h` (seeded by the game, edited via the ECS command ring, read by the renderer off the snapshot, serialized by `WorldManager`). The `GetFogSettings()` / `GetSkySettings()` RenderThread global statics are deleted. `world.json` gains a top-level `"Environment"` block; load is backward-compatible with old files. Time-of-day persistence is out of scope.

**Tech Stack:** C++23, custom ECS (`ecs.dll`), NVRHI deferred renderer (`Engine.dll`), nlohmann/json, Dear ImGui editor overlay, hot-reloaded `Game.dll`.

**Spec:** `docs/superpowers/specs/2026-05-25-persist-scene-atmosphere-design.md`

---

## Build & test reference (every task uses these)

Build preset is **`msvc-win64-vs2026-community`** (the enterprise preset is NOT installed — do not use it). Binaries land in `out/build/msvc-win64-vs2026-community/bin/Debug/`.

Common commands:

```powershell
# Configure (only needed once / after CMakeLists changes)
cmake --preset msvc-win64-vs2026-community

# Build a target (deps build automatically)
cmake --build --preset msvc-win64-vs2026-community --target <target>

# Run a unit test
./out/build/msvc-win64-vs2026-community/bin/Debug/<test>.exe
```

Targets you will build: `ecs`, `Engine`, `editor`, `game`, plus test targets `test_ecs`, `test_worldserial`.

**Commit identity (MANDATORY):** commit as `Nuno Silva <nuno.levezinho@live.com.pt>`. Use:

```powershell
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "<msg>"
```

Never use the work email. Never `git add .claude/`. Stage only the files you changed.

**Restart caveat:** Task 4 changes `ECS.h` layout and bumps `GAME_API_VERSION`. After Task 4 the running `editor.exe` must be restarted by the user — a subagent cannot restart it. Build everything, report that a restart + GUI smoke is required; do not block on it.

---

## File map

- `src/common/include/ECS.h` — add `FogComponent`, `SkyComponent`; register in the X-macro. (Task 1)
- `src/common/include/ECSCommands.h` — register both in `ApplyComponentCommand` + `RemoveComponentByType`. (Task 1)
- `src/common/include/ComponentSerialization.h` — **new.** Inline json (de)serializers for all components (moved out of `WorldManager.cpp`) + new Fog/Sky/DayNight + the `Environment` block helpers. (Tasks 2–3)
- `src/engine/src/utilities/WorldManager.cpp` — use the new header; write/read the `"Environment"` block. (Tasks 2–3)
- `tests/test_worldserial.cpp` — **new.** Round-trip + backward-compat + ComputeFog tests. (Tasks 2–3)
- `tests/CMakeLists.txt` — add `test_worldserial`. (Task 2)
- `src/engine/src/rendering/Fog.h` / `Fog.cpp` — `ComputeFog` takes `FogComponent`; drop `GetFogSettings`/`FogSettings`. (Task 4)
- `src/engine/src/rendering/Sky.h` / `Sky.cpp` — **deleted** (all content migrated). (Task 4)
- `src/engine/CMakeLists.txt` — drop `Sky.cpp` from the source list. (Task 4)
- `src/engine/src/rendering/Renderer.cpp` — read `FogComponent` from the snapshot singleton. (Task 4)
- `src/engine/src/rendering/passes/LightingRenderPass.cpp` — `FogEnabled` from `FogComponent`. (Task 4)
- `src/engine/src/rendering/passes/SkyRenderPass.cpp` — read `SkyComponent` from the snapshot singleton. (Task 4)
- `src/editor/src/rendering/imgui/RenderStatsPanel.cpp` — remove the Fog + Sky sections. (Task 4)
- `src/game/src/game.cpp` — seed `FogComponent` + `SkyComponent` singletons. (Task 4)
- `src/game/include/Game.h` — bump `GAME_API_VERSION` 5 → 6. (Task 4)
- `src/editor/src/rendering/imgui/DayNightPanel.cpp` — add Fog + Sky editing, rename window to "Atmosphere". (Task 5)

---

## Task 1: Fog/Sky as ECS components + command registration

**Files:**
- Modify: `src/common/include/ECS.h:142` (add structs after `DayNightConfigComponent`/`AtmosphereStateComponent`), `src/common/include/ECS.h:169` (X-macro)
- Modify: `src/common/include/ECSCommands.h:251-255` and `:273-275`

This task is structural (new component types). Verification is a successful `ecs` build — there is no unit to test yet; serialization comes in Task 2.

- [ ] **Step 1: Add `FogComponent` and `SkyComponent` to `ECS.h`**

In `src/common/include/ECS.h`, immediately after the `AtmosphereStateComponent` struct (currently ends at line 147, just before `struct AppControlComponent`), insert:

```cpp
// Scene-authored fog. Singleton component (one per world), serialized in world.json.
// Defaults reproduce the previous FogSettings values exactly.
struct FogComponent {
    bool      Enabled      = true;
    float     DayDensity   = 0.0f;                            // no fog at full day
    float     NightDensity = 0.09f;                           // noticeable at night
    glm::vec3 DayColor     = glm::vec3(0.60f, 0.70f, 0.80f);  // hazy blue-grey
    glm::vec3 NightColor   = glm::vec3(0.03f, 0.04f, 0.08f);  // dark blue
};

// Scene-authored procedural sky. Singleton component, serialized in world.json.
// Defaults reproduce the previous SkySettings values exactly.
struct SkyComponent {
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
```

- [ ] **Step 2: Register both in the X-macro**

In `src/common/include/ECS.h`, in `ECS_FOR_EACH_REGISTERED_COMPONENT`, add two lines after `X(AtmosphereStateComponent) \` (line 169):

```cpp
    X(DayNightConfigComponent) \
    X(AtmosphereStateComponent) \
    X(FogComponent) \
    X(SkyComponent) \
    X(AppControlComponent) \
    X(ViewportComponent)
```

(Only the two `X(FogComponent)` / `X(SkyComponent)` lines are new; the rest show context.)

- [ ] **Step 3: Register both in `ECSCommands.h` `ApplyComponentCommand`**

In `src/common/include/ECSCommands.h`, in `ApplyComponentCommand`, after the `DayNightConfigComponent` branch (ends line 255, before `// Add more component types as needed`), add:

```cpp
        } else if (componentData.Type == std::type_index(typeid(FogComponent))) {
            if (auto* fog = componentData.Get<FogComponent>()) {
                world.AddComponent(entity, *fog);
            }
        } else if (componentData.Type == std::type_index(typeid(SkyComponent))) {
            if (auto* sky = componentData.Get<SkyComponent>()) {
                world.AddComponent(entity, *sky);
            }
```

- [ ] **Step 4: Register both in `ECSCommands.h` `RemoveComponentByType`**

In the same file, in `RemoveComponentByType`, after the `DayNightConfigComponent` branch (line 274), add:

```cpp
        } else if (typeIndex == std::type_index(typeid(FogComponent))) {
            world.RemoveComponent<FogComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(SkyComponent))) {
            world.RemoveComponent<SkyComponent>(entity);
```

- [ ] **Step 5: Build `ecs` to verify the new component instantiations compile**

Run: `cmake --build --preset msvc-win64-vs2026-community --target ecs`
Expected: builds clean (the X-macro now instantiates `ComponentArray<FogComponent>` / `ComponentArray<SkyComponent>`).

- [ ] **Step 6: Commit**

```powershell
git add src/common/include/ECS.h src/common/include/ECSCommands.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(ecs): add FogComponent + SkyComponent singleton components"
```

---

## Task 2: Serialization header + component round-trip test

**Files:**
- Create: `src/common/include/ComponentSerialization.h`
- Modify: `src/engine/src/utilities/WorldManager.cpp:9-127` (delete the moved serializers, include the header)
- Create: `tests/test_worldserial.cpp`
- Modify: `tests/CMakeLists.txt` (append a `test_worldserial` target)

The component json (de)serializers currently live inside `WorldManager.cpp` and are unreachable from a unit test. Move them into a header (inline) so both `WorldManager.cpp` and the test use one copy, and add serializers for the three atmosphere components. TDD: write the test first.

- [ ] **Step 1: Write the failing test `tests/test_worldserial.cpp`**

```cpp
#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>

#include "ComponentSerialization.h" // inline json (de)serializers for components

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }
static bool veq(const glm::vec3& a, const glm::vec3& b) {
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

static void T00_fog_roundtrip()
{
    FogComponent in;
    in.Enabled = false;
    in.DayDensity = 0.012f;
    in.NightDensity = 0.21f;
    in.DayColor = glm::vec3(0.11f, 0.22f, 0.33f);
    in.NightColor = glm::vec3(0.44f, 0.55f, 0.66f);

    const nlohmann::json j = in;
    const auto out = j.get<FogComponent>();

    EXPECT(out.Enabled == in.Enabled);
    EXPECT(near(out.DayDensity, in.DayDensity));
    EXPECT(near(out.NightDensity, in.NightDensity));
    EXPECT(veq(out.DayColor, in.DayColor));
    EXPECT(veq(out.NightColor, in.NightColor));
}

static void T01_sky_roundtrip()
{
    SkyComponent in;
    in.Enabled = false;
    in.DayZenith = glm::vec3(0.1f, 0.2f, 0.3f);
    in.SunColor = glm::vec3(0.9f, 0.8f, 0.7f);
    in.SunRadiusDeg = 7.5f;
    in.SunGlow = 33.0f;
    in.MoonColor = glm::vec3(0.5f, 0.6f, 0.7f);
    in.MoonRadiusDeg = 4.25f;
    in.MoonGlow = 99.0f;

    const nlohmann::json j = in;
    const auto out = j.get<SkyComponent>();

    EXPECT(out.Enabled == in.Enabled);
    EXPECT(veq(out.DayZenith, in.DayZenith));
    EXPECT(veq(out.SunColor, in.SunColor));
    EXPECT(near(out.SunRadiusDeg, in.SunRadiusDeg));
    EXPECT(near(out.SunGlow, in.SunGlow));
    EXPECT(veq(out.MoonColor, in.MoonColor));
    EXPECT(near(out.MoonRadiusDeg, in.MoonRadiusDeg));
    EXPECT(near(out.MoonGlow, in.MoonGlow));
}

static void T02_daynight_roundtrip()
{
    DayNightConfigComponent in;
    in.CycleSeconds = 123.0f;
    in.DayBrightness = 0.7f;
    in.MoonIntensity = 0.25f;
    in.TwilightWidth = 0.4f;
    in.DayAmbient = 0.13f;
    in.MoonColor = glm::vec3(0.2f, 0.3f, 0.4f);

    const nlohmann::json j = in;
    const auto out = j.get<DayNightConfigComponent>();

    EXPECT(near(out.CycleSeconds, in.CycleSeconds));
    EXPECT(near(out.DayBrightness, in.DayBrightness));
    EXPECT(near(out.MoonIntensity, in.MoonIntensity));
    EXPECT(near(out.TwilightWidth, in.TwilightWidth));
    EXPECT(near(out.DayAmbient, in.DayAmbient));
    EXPECT(veq(out.MoonColor, in.MoonColor));
}

int main()
{
    T00_fog_roundtrip();
    T01_sky_roundtrip();
    T02_daynight_roundtrip();

    if (g_Failures == 0) { std::printf("All world-serialization tests passed.\n"); return 0; }
    std::printf("%d world-serialization test(s) FAILED.\n", g_Failures);
    return 1;
}
```

> Note: this test does NOT include `Fog.h` / compile `Fog.cpp` yet — `Fog.cpp` still exports `GetFogSettings` via `ENGINE_API` (a `dllimport` in a test exe, which cannot be defined locally), so it can only join the test in Task 4 after that global is removed. The `ComputeFog` test (`T05`) is added in Task 4.

- [ ] **Step 2: Add the `test_worldserial` target to `tests/CMakeLists.txt`**

Append at the end of `tests/CMakeLists.txt`:

```cmake
add_executable(test_worldserial
    test_worldserial.cpp
)

target_link_libraries(test_worldserial PRIVATE
    CommonHeaders
    glm::glm
    ecs
    nlohmann_json::nlohmann_json
)

target_include_directories(test_worldserial PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
)

target_compile_definitions(test_worldserial PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_worldserial PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 3: Configure + build to verify it FAILS to compile (header missing)**

Run:
```powershell
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_worldserial
```
Expected: FAIL — `cannot open source file "ComponentSerialization.h"` (the header does not exist yet).

- [ ] **Step 4: Create `src/common/include/ComponentSerialization.h`**

This moves the existing serializers out of `WorldManager.cpp` (verbatim, made `inline`) and adds the three atmosphere components. Full file:

```cpp
#pragma once

// Single home for the json (de)serialization of ECS component types, shared by
// WorldManager (save/load) and unit tests. All free functions are `inline` so the
// header can be included in multiple translation units without ODR violations.

#include <nlohmann/json.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "ECS.h"

namespace nlohmann {
    template<> struct adl_serializer<glm::vec3> {
        static void to_json(json& j, const glm::vec3& v) {
            j = json{{"X", v.x}, {"Y", v.y}, {"Z", v.z}};
        }
        static void from_json(const json& j, glm::vec3& v) {
            if (j.is_array() && j.size() == 3) {
                v.x = j[0].get<float>();
                v.y = j[1].get<float>();
                v.z = j[2].get<float>();
            } else {
                j.at("X").get_to(v.x);
                j.at("Y").get_to(v.y);
                j.at("Z").get_to(v.z);
            }
        }
    };

    template<> struct adl_serializer<glm::vec4> {
        static void to_json(json& j, const glm::vec4& v) {
            j = json{{"X", v.x}, {"Y", v.y}, {"Z", v.z}, {"W", v.w}};
        }
        static void from_json(const json& j, glm::vec4& v) {
            if (j.is_array() && j.size() == 4) {
                v.x = j[0].get<float>();
                v.y = j[1].get<float>();
                v.z = j[2].get<float>();
                v.w = j[3].get<float>();
            } else {
                j.at("X").get_to(v.x);
                j.at("Y").get_to(v.y);
                j.at("Z").get_to(v.z);
                j.at("W").get_to(v.w);
            }
        }
    };
}

inline void to_json(nlohmann::json& j, const glm::vec3& t) {
    j = nlohmann::json{{"X", t.x}, {"Y", t.y}, {"Z", t.z}};
}
inline void from_json(const nlohmann::json& j, glm::vec3& t) {
    j.at("X").get_to(t.x);
    j.at("Y").get_to(t.y);
    j.at("Z").get_to(t.z);
}
inline void to_json(nlohmann::json& j, const glm::vec4& t) {
    j = nlohmann::json{{"R", t.r}, {"G", t.g}, {"B", t.b}, {"A", t.a}};
}
inline void from_json(const nlohmann::json& j, glm::vec4& t) {
    j.at("R").get_to(t.r);
    j.at("G").get_to(t.g);
    j.at("B").get_to(t.b);
    j.at("A").get_to(t.a);
}

inline void to_json(nlohmann::json& j, const TransformComponent& t) {
    j = nlohmann::json{{"Position", t.Position}, {"Rotation", t.Rotation}, {"Scale", t.Scale}};
}
inline void from_json(const nlohmann::json& j, TransformComponent& t) {
    j.at("Position").get_to(t.Position);
    j.at("Rotation").get_to(t.Rotation);
    j.at("Scale").get_to(t.Scale);
}

inline void to_json(nlohmann::json& j, const MeshComponent& t) {
    j = nlohmann::json{{"MeshId", t.MeshId}, {"Visible", t.Visible}};
}
inline void from_json(const nlohmann::json& j, MeshComponent& t) {
    j.at("MeshId").get_to(t.MeshId);
    j.at("Visible").get_to(t.Visible);
}

inline void to_json(nlohmann::json& j, const MaterialComponent& t) {
    j = nlohmann::json{{"MaterialId", t.MaterialId}, {"BaseColor", t.BaseColor}, {"Flags", t.Flags}};
}
inline void from_json(const nlohmann::json& j, MaterialComponent& t) {
    j.at("MaterialId").get_to(t.MaterialId);
    j.at("BaseColor").get_to(t.BaseColor);
    j.at("Flags").get_to(t.Flags);
}

inline void to_json(nlohmann::json& j, const LightningComponent& t) {
    j = nlohmann::json{{"Type", t.Type}, {"Direction", t.Direction}, {"Color", t.Color}, {"Intensity", t.Intensity}, {"Range", t.Range}};
}
inline void from_json(const nlohmann::json& j, LightningComponent& t) {
    j.at("Type").get_to(t.Type);
    j.at("Direction").get_to(t.Direction);
    j.at("Color").get_to(t.Color);
    j.at("Intensity").get_to(t.Intensity);
    j.at("Range").get_to(t.Range);
}

inline void to_json(nlohmann::json& j, const TextComponent& t) {
    j = nlohmann::json{{"Text", t.Text}, {"Color", t.Color}, {"FontSize", t.FontSize}};
}
inline void from_json(const nlohmann::json& j, TextComponent& t) {
    j.at("Text").get_to(t.Text);
    j.at("Color").get_to(t.Color);
    j.at("FontSize").get_to(t.FontSize);
}

inline void to_json(nlohmann::json& j, const SunMarker&) { j = nlohmann::json::object(); }
inline void from_json(const nlohmann::json&, SunMarker&) {}

// ----- Atmosphere components (new) -----

inline void to_json(nlohmann::json& j, const FogComponent& t) {
    j = nlohmann::json{
        {"Enabled", t.Enabled},
        {"DayDensity", t.DayDensity},
        {"NightDensity", t.NightDensity},
        {"DayColor", t.DayColor},
        {"NightColor", t.NightColor}};
}
inline void from_json(const nlohmann::json& j, FogComponent& t) {
    j.at("Enabled").get_to(t.Enabled);
    j.at("DayDensity").get_to(t.DayDensity);
    j.at("NightDensity").get_to(t.NightDensity);
    j.at("DayColor").get_to(t.DayColor);
    j.at("NightColor").get_to(t.NightColor);
}

inline void to_json(nlohmann::json& j, const SkyComponent& t) {
    j = nlohmann::json{
        {"Enabled", t.Enabled},
        {"DayZenith", t.DayZenith},
        {"DayHorizon", t.DayHorizon},
        {"NightZenith", t.NightZenith},
        {"NightHorizon", t.NightHorizon},
        {"SunColor", t.SunColor},
        {"SunRadiusDeg", t.SunRadiusDeg},
        {"SunGlow", t.SunGlow},
        {"MoonColor", t.MoonColor},
        {"MoonRadiusDeg", t.MoonRadiusDeg},
        {"MoonGlow", t.MoonGlow}};
}
inline void from_json(const nlohmann::json& j, SkyComponent& t) {
    j.at("Enabled").get_to(t.Enabled);
    j.at("DayZenith").get_to(t.DayZenith);
    j.at("DayHorizon").get_to(t.DayHorizon);
    j.at("NightZenith").get_to(t.NightZenith);
    j.at("NightHorizon").get_to(t.NightHorizon);
    j.at("SunColor").get_to(t.SunColor);
    j.at("SunRadiusDeg").get_to(t.SunRadiusDeg);
    j.at("SunGlow").get_to(t.SunGlow);
    j.at("MoonColor").get_to(t.MoonColor);
    j.at("MoonRadiusDeg").get_to(t.MoonRadiusDeg);
    j.at("MoonGlow").get_to(t.MoonGlow);
}

inline void to_json(nlohmann::json& j, const DayNightConfigComponent& t) {
    j = nlohmann::json{
        {"CycleSeconds", t.CycleSeconds},
        {"DayBrightness", t.DayBrightness},
        {"MoonIntensity", t.MoonIntensity},
        {"TwilightWidth", t.TwilightWidth},
        {"DayAmbient", t.DayAmbient},
        {"MoonColor", t.MoonColor}};
}
inline void from_json(const nlohmann::json& j, DayNightConfigComponent& t) {
    j.at("CycleSeconds").get_to(t.CycleSeconds);
    j.at("DayBrightness").get_to(t.DayBrightness);
    j.at("MoonIntensity").get_to(t.MoonIntensity);
    j.at("TwilightWidth").get_to(t.TwilightWidth);
    j.at("DayAmbient").get_to(t.DayAmbient);
    j.at("MoonColor").get_to(t.MoonColor);
}
```

- [ ] **Step 5: Build + run the test → PASS**

Run:
```powershell
cmake --build --preset msvc-win64-vs2026-community --target test_worldserial
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
```
Expected: `All world-serialization tests passed.`

- [ ] **Step 6: Refactor `WorldManager.cpp` to use the header (remove the now-duplicate definitions)**

In `src/engine/src/utilities/WorldManager.cpp`, delete the entire block from line 11 (`#include <glm/vec3.hpp>`) through line 127 (the `SunMarker` serializers) — i.e. every `namespace nlohmann { ... }` specialization and every free `to_json`/`from_json` — and replace it with a single include. The top of the file becomes:

```cpp
#include "WorldManager.h"

#include <fstream>

#include "lib.h"

#include <nlohmann/json.hpp>

#include "ComponentSerialization.h" // shared component json (de)serializers

using json = nlohmann::json;

bool WorldManager::SaveWorldSnapshot(const std::string& filepath, const ECS* world) {
    // ... unchanged ...
```

Leave `SaveWorldSnapshot` / `LoadWorldSnapshot` bodies unchanged in this task (the `"Environment"` block is added in Task 3).

- [ ] **Step 7: Build `Engine` to verify the refactor compiles + run the test again**

Run:
```powershell
cmake --build --preset msvc-win64-vs2026-community --target Engine
cmake --build --preset msvc-win64-vs2026-community --target test_worldserial
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
```
Expected: `Engine` builds clean; test prints `All world-serialization tests passed.`

- [ ] **Step 8: Commit**

```powershell
git add src/common/include/ComponentSerialization.h src/engine/src/utilities/WorldManager.cpp tests/test_worldserial.cpp tests/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "refactor(serial): shared ComponentSerialization.h + Fog/Sky/DayNight serializers"
```

---

## Task 3: `Environment` block helpers + save/load wiring

**Files:**
- Modify: `src/common/include/ComponentSerialization.h` (append `Environment` helpers)
- Modify: `tests/test_worldserial.cpp` (add helper tests + ComputeFog placeholder note)
- Modify: `src/engine/src/utilities/WorldManager.cpp` (`SaveWorldSnapshot` + `LoadWorldSnapshot`)

The save/load glue needs an ECS instance (`GetSingleton`/`SetSingleton`), so factor the json side into pure helpers that the test can exercise without an ECS.

- [ ] **Step 1: Add failing helper tests to `tests/test_worldserial.cpp`**

Add these two functions before `main()` and call them from `main()`:

```cpp
static void T03_environment_roundtrip()
{
    FogComponent fog; fog.NightDensity = 0.17f; fog.DayColor = glm::vec3(0.1f, 0.2f, 0.3f);
    SkyComponent sky; sky.SunGlow = 42.0f; sky.MoonColor = glm::vec3(0.4f, 0.5f, 0.6f);
    DayNightConfigComponent dn; dn.CycleSeconds = 77.0f; dn.DayAmbient = 0.2f;

    nlohmann::json root;
    root["Entities"] = nlohmann::json::array();
    root["Environment"] = BuildEnvironmentJson(fog, sky, dn);

    const EnvironmentData env = ParseEnvironmentJson(root);
    EXPECT(env.HasFog && env.HasSky && env.HasDayNight);
    EXPECT(near(env.Fog.NightDensity, fog.NightDensity));
    EXPECT(veq(env.Fog.DayColor, fog.DayColor));
    EXPECT(near(env.Sky.SunGlow, sky.SunGlow));
    EXPECT(veq(env.Sky.MoonColor, sky.MoonColor));
    EXPECT(near(env.DayNight.CycleSeconds, dn.CycleSeconds));
    EXPECT(near(env.DayNight.DayAmbient, dn.DayAmbient));
}

static void T04_environment_absent_is_backward_compatible()
{
    // An old world.json: entities only, no "Environment" key.
    nlohmann::json root;
    root["EntityCount"] = 0;
    root["Entities"] = nlohmann::json::array();

    const EnvironmentData env = ParseEnvironmentJson(root); // must not throw
    EXPECT(!env.HasFog);
    EXPECT(!env.HasSky);
    EXPECT(!env.HasDayNight);
}
```

In `main()`, add after `T02_daynight_roundtrip();`:

```cpp
    T03_environment_roundtrip();
    T04_environment_absent_is_backward_compatible();
```

- [ ] **Step 2: Build → FAIL (helpers undefined)**

Run:
```powershell
cmake --build --preset msvc-win64-vs2026-community --target test_worldserial
```
Expected: FAIL — `BuildEnvironmentJson` / `ParseEnvironmentJson` / `EnvironmentData` not declared.

- [ ] **Step 3: Add the `Environment` helpers to `ComponentSerialization.h`**

Append at the end of `src/common/include/ComponentSerialization.h`:

```cpp
// ----- world.json top-level "Environment" block -----

// Build the "Environment" object value (NOT wrapped — assign to root["Environment"]).
inline nlohmann::json BuildEnvironmentJson(const FogComponent& fog,
                                           const SkyComponent& sky,
                                           const DayNightConfigComponent& dayNight) {
    return nlohmann::json{
        {"Fog", fog},
        {"Sky", sky},
        {"DayNight", dayNight}};
}

// Parsed result of a root document's "Environment" block. Each Has* flag is true
// only if that sub-object was present; the corresponding value is otherwise left
// at its default. Absent "Environment" (old files) => all flags false.
struct EnvironmentData {
    bool HasFog = false;
    bool HasSky = false;
    bool HasDayNight = false;
    FogComponent Fog;
    SkyComponent Sky;
    DayNightConfigComponent DayNight;
};

inline EnvironmentData ParseEnvironmentJson(const nlohmann::json& root) {
    EnvironmentData e;
    if (!root.contains("Environment")) return e;
    const auto& env = root.at("Environment");
    if (env.contains("Fog"))      { e.Fog      = env.at("Fog").get<FogComponent>();             e.HasFog = true; }
    if (env.contains("Sky"))      { e.Sky      = env.at("Sky").get<SkyComponent>();             e.HasSky = true; }
    if (env.contains("DayNight")) { e.DayNight = env.at("DayNight").get<DayNightConfigComponent>(); e.HasDayNight = true; }
    return e;
}
```

- [ ] **Step 4: Build + run test → PASS**

Run:
```powershell
cmake --build --preset msvc-win64-vs2026-community --target test_worldserial
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
```
Expected: `All world-serialization tests passed.`

- [ ] **Step 5: Write the `"Environment"` block in `SaveWorldSnapshot`**

In `src/engine/src/utilities/WorldManager.cpp`, in `SaveWorldSnapshot`, after the entities loop closes and before `ofs << j.dump(4);` (currently around line 169-171), insert:

```cpp
    // Top-level scene atmosphere (singletons live on the hidden reserved entity, so
    // they are not in the Entities array). Defaults if a singleton is somehow absent.
    {
        FogComponent fog{};
        SkyComponent sky{};
        DayNightConfigComponent dayNight{};
        if (const auto* f = world->GetSingleton<FogComponent>())            fog = *f;
        if (const auto* s = world->GetSingleton<SkyComponent>())            sky = *s;
        if (const auto* d = world->GetSingleton<DayNightConfigComponent>()) dayNight = *d;
        j["Environment"] = BuildEnvironmentJson(fog, sky, dayNight);
    }
```

- [ ] **Step 6: Apply the `"Environment"` block in `LoadWorldSnapshot`**

In the same file, in `LoadWorldSnapshot`, inside the `try` block, after the entity-creation `for` loop closes (currently around line 210, before the closing `}` of `try`), insert:

```cpp
        // Apply scene atmosphere if present. Singletons survive Clear(), so when the
        // block is absent (old world.json) the seeded defaults are left untouched.
        const EnvironmentData env = ParseEnvironmentJson(j);
        if (env.HasFog)      world->SetSingleton(env.Fog);
        if (env.HasSky)      world->SetSingleton(env.Sky);
        if (env.HasDayNight) world->SetSingleton(env.DayNight);
```

- [ ] **Step 7: Build `Engine` to verify the wiring compiles**

Run: `cmake --build --preset msvc-win64-vs2026-community --target Engine`
Expected: builds clean.

- [ ] **Step 8: Commit**

```powershell
git add src/common/include/ComponentSerialization.h src/engine/src/utilities/WorldManager.cpp tests/test_worldserial.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(scene): persist Fog/Sky/DayNight in world.json Environment block"
```

---

## Task 4: Renderer cutover — read components, delete the global statics

**Files:**
- Modify: `src/engine/src/rendering/Fog.h`, `src/engine/src/rendering/Fog.cpp`
- Delete: `src/engine/src/rendering/Sky.h`, `src/engine/src/rendering/Sky.cpp`
- Modify: `src/engine/CMakeLists.txt:26` (remove `Sky.cpp`)
- Modify: `src/engine/src/rendering/Renderer.cpp:255-261`
- Modify: `src/engine/src/rendering/passes/LightingRenderPass.cpp:244`
- Modify: `src/engine/src/rendering/passes/SkyRenderPass.cpp:4,115,154-168`
- Modify: `src/editor/src/rendering/imgui/RenderStatsPanel.cpp:6-7,38-60`
- Modify: `src/game/src/game.cpp:216-217`
- Modify: `src/game/include/Game.h:20`

This is the atomic cutover: the `GetFogSettings()`/`GetSkySettings()` globals are removed, so every consumer must change in the same task or the build breaks. After this task, fog/sky still render (read from components) but have no editor UI until Task 5. This task has no new unit test — verification is a clean full build + the existing `test_worldserial` + `test_ecs` staying green; visual correctness is the user's GUI smoke after restart.

- [ ] **Step 1: Rewrite `src/engine/src/rendering/Fog.h`**

Replace the whole file with:

```cpp
#pragma once

#include <glm/vec3.hpp>

#include "ECS.h" // FogComponent

// Resolved fog for one frame: the color used for BOTH the scene clear and the
// geometry blend, plus the exponential density.
struct FogFrame {
    glm::vec3 Color   = glm::vec3(0.0f);
    float     Density = 0.0f;
};

// Pure: maps the directional sun's elevation to fog color + density.
// elevation = clamp(-sunDir.y, 0, 1): 1 at noon, 0 at/below the horizon (night).
FogFrame ComputeFog(const glm::vec3& sunDir, const FogComponent& fog);
```

- [ ] **Step 2: Rewrite `src/engine/src/rendering/Fog.cpp`**

Replace the whole file with:

```cpp
#include "Fog.h"

#include <glm/common.hpp> // glm::clamp, glm::mix

FogFrame ComputeFog(const glm::vec3& sunDir, const FogComponent& s)
{
    const float elevation = glm::clamp(-sunDir.y, 0.0f, 1.0f);
    FogFrame f;
    f.Density = glm::mix(s.NightDensity, s.DayDensity, elevation);
    f.Color   = glm::mix(s.NightColor,   s.DayColor,   elevation);
    return f;
}
```

- [ ] **Step 3: Delete `Sky.h` and `Sky.cpp`, drop `Sky.cpp` from the engine source list**

Delete the files:
```powershell
Remove-Item src/engine/src/rendering/Sky.h, src/engine/src/rendering/Sky.cpp
```

In `src/engine/CMakeLists.txt`, remove the `src/rendering/Sky.cpp` line (line 26). Keep `src/rendering/Fog.cpp` (line 25).

- [ ] **Step 4: `Renderer.cpp` — read `FogComponent` from the snapshot singleton**

In `src/engine/src/rendering/Renderer.cpp`, replace lines 255-261 (`const FogSettings& fogSettings = GetFogSettings();` through the `sceneClear` ternary) with:

```cpp
                FogComponent fogComp{};
                if (world) {
                    if (const auto* f = world->GetSingleton<FogComponent>()) fogComp = *f;
                }
                // Always resolve fog: LightingRenderPass reads m_FrameFog regardless of Enabled.
                m_FrameFog = ComputeFog(sunDir, fogComp);

                const auto sceneClear = fogComp.Enabled
                    ? nvrhi::Color(m_FrameFog.Color.r, m_FrameFog.Color.g, m_FrameFog.Color.b, 1.0f)
                    : nvrhi::Color(red, green, blue, 1.0f);
```

`Renderer.cpp` already includes `Fog.h` (via `Renderer.h`) and has `world` in scope here (see `Renderer::Render` signature, line 202). No new include needed. If a compile error reports `GetFogSettings` still referenced elsewhere in `Renderer.cpp`, grep the file and remove that usage too.

- [ ] **Step 5: `LightingRenderPass.cpp` — `FogEnabled` from `FogComponent`**

In `src/engine/src/rendering/passes/LightingRenderPass.cpp`, replace line 244 (`cb.FogEnabled = GetFogSettings().Enabled ? 1 : 0;`) with:

```cpp
    bool fogEnabled = true;
    if (world) {
        if (const auto* f = world->GetSingleton<FogComponent>()) fogEnabled = f->Enabled;
    }
    cb.FogEnabled = fogEnabled ? 1 : 0;
```

`world` is a parameter of this `Render` method and `AtmosphereStateComponent` is already read the same way (line 233), so `ECS.h` is already included. If `LightingRenderPass.cpp` has an `#include "Fog.h"`, keep it (still needed for `FogFrame`/`ComputeFog` via `GetFrameFog`).

- [ ] **Step 6: `SkyRenderPass.cpp` — read `SkyComponent` from the snapshot singleton**

In `src/engine/src/rendering/passes/SkyRenderPass.cpp`:

(a) Line 4: change `#include "Sky.h"` to `#include "ECS.h"`.

(b) Replace the early `Enabled` gate at the top of `Render` (lines 115-116):

```cpp
    if (!GetSkySettings().Enabled)
        return;
```

with a read of the singleton (default if absent), gating on it:

```cpp
    SkyComponent s{};
    if (world) {
        if (const auto* sk = world->GetSingleton<SkyComponent>()) s = *sk;
    }
    if (!s.Enabled)
        return;
```

(c) Remove the later `const SkySettings& s = GetSkySettings();` line (currently line 154) — `s` is now the `SkyComponent` read above. The CB-fill block (lines 155-168) references `s.DayZenith`, `s.SunColor`, `s.SunGlow`, etc.; `SkyComponent` has identical field names, so those lines compile unchanged once the old `s` declaration is removed.

- [ ] **Step 7: `RenderStatsPanel.cpp` — remove the Fog + Sky sections**

In `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`:

(a) Remove the includes at lines 6-7:
```cpp
#include "Fog.h" // resolved via the editor's Engine PUBLIC include dirs (same as RenderStats.h)
#include "Sky.h"
```

(b) Remove everything from line 38 (`ImGui::Separator();` that precedes `TextDisabled("Fog")`) through line 60 (the last Sky `SliderFloat`), so the panel ends after the Shadows section. The function tail becomes:

```cpp
    ImGui::Separator();
    ImGui::TextDisabled("Shadows");
    ShadowSettings& sh = GetShadowSettings();
    ImGui::Checkbox("Shadows", &sh.Enabled);
    ImGui::SliderFloat("Shadow bias", &sh.Bias, 0.0f, 0.01f, "%.4f");

    ImGui::End();
}
```

- [ ] **Step 8: `game.cpp` — seed the Fog/Sky singletons**

In `src/game/src/game.cpp`, after the existing singleton seeds (line 216-217, `SetSingleton(DayNightConfigComponent{})` / `SetSingleton(AtmosphereStateComponent{})`), add:

```cpp
	        g_GameState->World.SetSingleton(FogComponent{});
	        g_GameState->World.SetSingleton(SkyComponent{});
```

(Match the surrounding tab indentation.)

- [ ] **Step 9: `Game.h` — bump the API version**

In `src/game/include/Game.h`, change line 20:

```cpp
#define GAME_API_VERSION 6u
```

- [ ] **Step 10: Wire the ComputeFog unit test (now that `Fog.cpp` is ENGINE_API-free)**

`Fog.cpp` no longer exports anything, so it can be compiled directly into the test exe.

(a) In `tests/CMakeLists.txt`, in the `test_worldserial` target, add `Fog.cpp` to the sources and the rendering include dir:

```cmake
add_executable(test_worldserial
    test_worldserial.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering/Fog.cpp
)

target_link_libraries(test_worldserial PRIVATE
    CommonHeaders
    glm::glm
    ecs
    nlohmann_json::nlohmann_json
)

target_include_directories(test_worldserial PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering
)
```

(b) In `tests/test_worldserial.cpp`, add the `Fog.h` include after the `ComponentSerialization.h` include:

```cpp
#include "ComponentSerialization.h" // inline json (de)serializers for components
#include "Fog.h"                     // ComputeFog(const glm::vec3&, const FogComponent&)
```

(c) Add this test function before `main()` and call it from `main()` after `T04_...`:

```cpp
static void T05_computefog_day_vs_night()
{
    FogComponent fog; // defaults: DayDensity 0, NightDensity 0.09, day/night colors
    // Sun overhead -> sunDir points down (-y): elevation = 1 -> day endpoints.
    const FogFrame day = ComputeFog(glm::vec3(0, -1, 0), fog);
    // Sun below horizon -> sunDir points up (+y): elevation = 0 -> night endpoints.
    const FogFrame night = ComputeFog(glm::vec3(0, 1, 0), fog);

    EXPECT(near(day.Density, fog.DayDensity));
    EXPECT(veq(day.Color, fog.DayColor));
    EXPECT(near(night.Density, fog.NightDensity));
    EXPECT(veq(night.Color, fog.NightColor));
    EXPECT(night.Density > day.Density); // night is foggier with default values
}
```

In `main()`:

```cpp
    T05_computefog_day_vs_night();
```

- [ ] **Step 11: Full build (ecs → Engine → editor → game) + tests**

Run:
```powershell
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target editor
cmake --build --preset msvc-win64-vs2026-community --target game
cmake --build --preset msvc-win64-vs2026-community --target test_worldserial
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `editor` + `game` build clean (no references to `GetFogSettings`/`GetSkySettings`/`SkySettings`/`FogSettings` remain anywhere — if the compiler/linker complains, grep the whole repo for the symbol and fix the straggler). Both tests print their `All … passed.` lines (`test_worldserial` now runs T00–T05).

- [ ] **Step 12: Commit**

```powershell
git add src/engine/src/rendering/Fog.h src/engine/src/rendering/Fog.cpp src/engine/CMakeLists.txt src/engine/src/rendering/Renderer.cpp src/engine/src/rendering/passes/LightingRenderPass.cpp src/engine/src/rendering/passes/SkyRenderPass.cpp src/editor/src/rendering/imgui/RenderStatsPanel.cpp src/game/src/game.cpp src/game/include/Game.h tests/CMakeLists.txt tests/test_worldserial.cpp
git rm src/engine/src/rendering/Sky.h src/engine/src/rendering/Sky.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(render): drive Fog/Sky from ECS singletons; remove global statics"
```

> After this commit the running editor must be restarted by the user (ECS.h layout + `GAME_API_VERSION` changed). Report this; do not attempt to restart it.

---

## Task 5: Atmosphere panel — Fog/Sky editing via ECSCommand

**Files:**
- Modify: `src/editor/src/rendering/imgui/DayNightPanel.cpp`

Fog/Sky lost their UI in Task 4. Add them to `DayNightPanel`, edited through the ECS command ring (RenderThread → GameThread), mirroring the existing `DayNightConfigComponent` flow, and rename the window to "Atmosphere".

- [ ] **Step 1: Rewrite `src/editor/src/rendering/imgui/DayNightPanel.cpp`**

Replace the whole file with:

```cpp
#include "DayNightPanel.h"
#include "EditorContext.h"

#include <imgui.h>

#include "ECS.h"
#include "ECSCommands.h"
#include "ApplicationContext.h" // ctx.App->ECSCommandRing
#include "lib.h"                // SM_WARN

namespace {
    // Push a singleton-component edit through the command ring (RenderThread ->
    // GameThread), matching the DayNightConfig flow. Logs on ring-full.
    template <typename T>
    void PushSingletonEdit(const EditorContext& ctx, const ECS* world, const T& value, const char* what)
    {
        ECSCommand cmd = ECSCommand::AddComponent(world->SingletonEntity(), value);
        if (!ctx.App->ECSCommandRing.Push(cmd)) {
            SM_WARN("AtmospherePanel: ECSCommandRing full, %s edit dropped", what);
        }
    }
}

void DrawDayNightPanel(const EditorContext& ctx, bool* open)
{
    if (open && !*open) return;
    if (!ImGui::Begin("Atmosphere", open)) { ImGui::End(); return; }

    const ECS* world = ctx.World;
    if (!world) { ImGui::TextDisabled("No world"); ImGui::End(); return; }

    // --- Day / Night ---
    if (const DayNightConfigComponent* cur = world->GetSingleton<DayNightConfigComponent>()) {
        ImGui::SeparatorText("Day / Night");
        DayNightConfigComponent cfg = *cur;
        bool changed = false;
        changed |= ImGui::SliderFloat("Cycle seconds", &cfg.CycleSeconds, 2.0f, 300.0f, "%.1f");
        changed |= ImGui::SliderFloat("Day brightness", &cfg.DayBrightness, 0.0f, 1.0f, "%.2f");
        changed |= ImGui::SliderFloat("Moon intensity", &cfg.MoonIntensity, 0.0f, 1.0f, "%.3f");
        changed |= ImGui::SliderFloat("Twilight width", &cfg.TwilightWidth, 0.01f, 1.0f, "%.2f");
        changed |= ImGui::SliderFloat("Day ambient", &cfg.DayAmbient, 0.0f, 0.5f, "%.3f");
        changed |= ImGui::ColorEdit3("Moon color (fill)", &cfg.MoonColor.x);
        if (changed) PushSingletonEdit(ctx, world, cfg, "day/night");
    } else {
        ImGui::TextDisabled("No DayNightConfig singleton");
    }

    // --- Fog ---
    if (const FogComponent* cur = world->GetSingleton<FogComponent>()) {
        ImGui::SeparatorText("Fog");
        FogComponent fog = *cur;
        bool changed = false;
        changed |= ImGui::Checkbox("Fog enabled", &fog.Enabled);
        changed |= ImGui::SliderFloat("Day density",   &fog.DayDensity,   0.0f, 0.05f, "%.4f");
        changed |= ImGui::SliderFloat("Night density", &fog.NightDensity, 0.0f, 0.30f, "%.3f");
        changed |= ImGui::ColorEdit3("Day color",   &fog.DayColor.x);
        changed |= ImGui::ColorEdit3("Night color", &fog.NightColor.x);
        if (changed) PushSingletonEdit(ctx, world, fog, "fog");
    } else {
        ImGui::TextDisabled("No Fog singleton");
    }

    // --- Sky ---
    if (const SkyComponent* cur = world->GetSingleton<SkyComponent>()) {
        ImGui::SeparatorText("Sky");
        SkyComponent sky = *cur;
        bool changed = false;
        changed |= ImGui::Checkbox("Sky enabled", &sky.Enabled);
        changed |= ImGui::ColorEdit3("Day zenith",    &sky.DayZenith.x);
        changed |= ImGui::ColorEdit3("Day horizon",   &sky.DayHorizon.x);
        changed |= ImGui::ColorEdit3("Night zenith",  &sky.NightZenith.x);
        changed |= ImGui::ColorEdit3("Night horizon", &sky.NightHorizon.x);
        changed |= ImGui::ColorEdit3("Sun color",     &sky.SunColor.x);
        changed |= ImGui::SliderFloat("Sun radius (deg)",  &sky.SunRadiusDeg, 0.5f, 15.0f, "%.1f");
        changed |= ImGui::SliderFloat("Sun glow",          &sky.SunGlow, 1.0f, 512.0f, "%.0f");
        changed |= ImGui::ColorEdit3("Moon color",    &sky.MoonColor.x);
        changed |= ImGui::SliderFloat("Moon radius (deg)", &sky.MoonRadiusDeg, 0.5f, 15.0f, "%.1f");
        changed |= ImGui::SliderFloat("Moon glow",         &sky.MoonGlow, 1.0f, 512.0f, "%.0f");
        if (changed) PushSingletonEdit(ctx, world, sky, "sky");
    } else {
        ImGui::TextDisabled("No Sky singleton");
    }

    ImGui::End();
}
```

> If `ImGui::SeparatorText` is unavailable in the vendored ImGui version, the build will error on that call; replace each `ImGui::SeparatorText("X");` with `ImGui::Separator(); ImGui::TextDisabled("X");` (the pattern the old RenderStatsPanel used).

- [ ] **Step 2: Build `editor`**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: builds clean.

- [ ] **Step 3: Commit**

```powershell
git add src/editor/src/rendering/imgui/DayNightPanel.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(editor): Atmosphere panel (Fog/Sky/DayNight) edits via ECS command ring"
```

---

## Done criteria

- `test_worldserial` and `test_ecs` print their `All … passed.` lines.
- `editor`, `runtime`-side `Engine`, `game` build clean; no references to `GetFogSettings`/`GetSkySettings`/`FogSettings`/`SkySettings` remain (grep the repo).
- User restarts the editor and GUI-smokes: tweak Fog/Sky/DayNight in the Atmosphere panel → Save scene → New/Reload → values persist and render; loading an old `world.json` (no `Environment` block) still loads with default atmosphere and does not crash.
```
