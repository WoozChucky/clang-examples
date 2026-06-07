# Migrate MenuButtonComponent to Game.dll Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move `MenuButtonComponent` from an engine builtin to a `Game.dll`-owned non-builtin component, edited via a game `editorDraw` hook (named action dropdown + 3 color pickers), which requires a new reusable `EditorUI::ComboMapped` primitive and extracting the glm-json helpers so a game header can serialize `glm::vec4` without depending on `ECS.h`.

**Architecture:** Four tasks: (1) extract the glm vec3/vec4 JSON helpers into an ECS-free `GlmJson.h`; (2) add the `EditorUI::ComboMapped` bridge primitive; (3) atomically move `MenuButtonComponent` + `Actions.h` into `Game.dll`, register with the hook, delete `MenuButtonEditor`, remove typed branches; (4) full regression. Follows the established `PlayerComponent` migration pattern.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-enterprise`), CMake, nlohmann/json, glm, Dear ImGui (editor only). Builds on the editor-hook (`RegisterEditorHook`/`EditorUI`), reload-survival (`GameRegisterComponents` + byte path), and registry-driven duplicate (`copyTo`) features already on `main`.

**Scope:** Implements `docs/superpowers/specs/2026-06-07-menubutton-component-to-gamedll-design.md`. Touches `ECS.h` (X-macro) ⇒ rebuild **ecs + Engine + editor + game** and **restart the editor**. No `GAME_API_VERSION` bump.

> **Branch:** Work happens on `feat/menubutton-to-gamedll` (already created off `main`). Stay on it.

---

## Background facts (verified)

- **Line numbers below are approximate** (this branch already has the `PlayerComponent` migration merged, which shifted some lines). **Locate every edit by the quoted code, not by line number.**
- `MenuButtonComponent` is in `src/common/include/ECS.h` (struct `{ uint32_t ActionId; glm::vec4 Normal/Hover/Press; }`) and in the `ECS_FOR_EACH_REGISTERED_COMPONENT` X-macro (`X(MenuButtonComponent)`). Its `to_json`/`from_json` are in `src/common/include/ComponentSerialization.h`. Its builtin registration is `r.Register<MenuButtonComponent>("MenuButtonComponent", true);` in `src/ecs/src/ComponentSerializers.cpp`.
- `ECSCommands.h` has a `MenuButtonComponent` typed branch in `ApplyComponentCommand` and in `RemoveComponentByType`. (Duplicate is already registry-driven via `copyTo` from the Player migration — `DuplicateEntityComponents` no longer names components.)
- The dedicated editor `src/editor/src/panels/inspector/MenuButtonEditor.{cpp,h}` shows `ActionId` as a named dropdown using `kActionNames = {"None","Play","Quit","Back"}` / `kActionIds = {Actions::None, Actions::Play, Actions::Quit, Actions::Back}`, plus three `ImGui::ColorEdit4`. It is registered in `src/editor/src/panels/EcsInspectorPanel.cpp` (`#include "inspector/MenuButtonEditor.h"` + `m_Editors.push_back(std::make_unique<MenuButtonEditor>());`) and listed in `src/editor/CMakeLists.txt` (`src/panels/inspector/MenuButtonEditor.cpp`).
- `src/common/include/Actions.h` is pure `constexpr uint32_t` action ids (`Actions::None/Play/Quit/Back`) with NO dependency on `MenuButtonComponent`. Consumers: `src/game/src/game.cpp` (includes `"Actions.h"`) and the soon-deleted `MenuButtonEditor.cpp`. `ActionEvent`/`ActionQueueComponent` (in `ECS.h`) use `ActionId` only as `uint32_t`.
- glm JSON helpers live in `ComponentSerialization.h` (which `#include`s `ECS.h`): a `namespace nlohmann { adl_serializer<glm::vec3>; adl_serializer<glm::vec4>; }` block (vec3 → `{X,Y,Z}`, vec4 → `{X,Y,Z,W}`) plus four free `to_json`/`from_json` for `glm::vec3` (`{X,Y,Z}`) and `glm::vec4` (`{R,G,B,A}`). `nlohmann` uses the `adl_serializer<glm::vec4>` specialization (`{X,Y,Z,W}`) for `j = vec4` / `j.get<vec4>()`, which is what `MenuButtonComponent`'s colors use.
- `EditorUI` (`src/common/include/EditorUI.h`) is a struct of fn-ptrs; `EditorUIImpl.cpp` defines `UI_*` functions and assembles them into `EditorUIInstance()` **in struct-declaration order**. Current `UI_Combo`:
  ```cpp
  bool UI_Combo(nlohmann::json& o, const char* k, const char* const* labels, int count) {
      if (!o.contains(k) || !o[k].is_number_integer()) return false;
      int v = o[k].get<int>();
      if (v < 0 || v >= count) return false;
      if (ImGui::Combo(k, &v, labels, count)) { o[k] = v; return true; }
      return false;
  }
  ```
- `game.cpp` seeds menu buttons with `MenuButtonComponent{ .ActionId = ... }` in several places and uses `Each<MenuButtonComponent, ...>` in systems — all in `src/game`, so they follow the new header automatically once the include is added.
- `MenuButtonComponent` is trivially copyable (uint32 + glm::vec4×3) → byte reload path + registry `copyTo` both apply automatically once registered (non-builtin).

## Type/symbol contract (keep exact)

- New `src/common/include/GlmJson.h`: the glm vec3/vec4 JSON helpers (ECS-free).
- New `EditorUI` member (LAST): `bool (*ComboMapped)(nlohmann::json& obj, const char* key, const char* const* labels, const int* values, int count);`.
- New `src/game/src/MenuButtonComponent.h`: struct + `to_json`/`from_json`.
- `Actions.h` relocated to `src/game/src/Actions.h`.

---

### Task 1: Extract glm-json helpers into `GlmJson.h` (behavior-preserving)

**Files:**
- Create: `src/common/include/GlmJson.h`
- Modify: `src/common/include/ComponentSerialization.h`

No new test (pure refactor; covered by the existing `test_worldserial`/`test_compserial` glm round-trips). Build + existing suites are the gate.

- [ ] **Step 1: Create `src/common/include/GlmJson.h`** with the glm JSON helpers moved verbatim:
```cpp
#pragma once

#include <nlohmann/json.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// JSON (de)serialization for glm vector types. Kept ECS-free (no ECS.h include) so that
// Game.dll-owned component headers can serialize glm vectors without pulling in ECS.h.
// Shared by ComponentSerialization.h (engine/common) and game component headers.
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
```
**IMPORTANT:** Read the current top of `ComponentSerialization.h` first and copy the glm helper definitions VERBATIM (the snippet above must match what's there; if any detail differs, the file is the source of truth — preserve exact behavior).

- [ ] **Step 2: Replace the helpers in `ComponentSerialization.h` with the include**

In `src/common/include/ComponentSerialization.h`, DELETE the entire `namespace nlohmann { adl_serializer<glm::vec3> ...; adl_serializer<glm::vec4> ...; }` block AND the four following free `to_json`/`from_json` for `glm::vec3`/`glm::vec4` (everything you copied into `GlmJson.h`). In their place (after the existing includes, before the first ECS-component serializer), add:
```cpp
#include "GlmJson.h"   // glm vec3/vec4 JSON helpers (extracted; ECS-free)
```
Leave the existing `#include <glm/vec3.hpp>` / `#include <glm/vec4.hpp>` / `#include <nlohmann/json.hpp>` / `#include "ECS.h"` in place (harmless; `GlmJson.h` also includes the glm/nlohmann ones).

- [ ] **Step 3: Build + run — GREEN (behavior preserved)**
```
cmake --build --preset msvc-win64-vs2026-enterprise --target ecs --target test_worldserial --target test_compserial
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_compserial.exe
```
Expected: both print their pass lines (glm vec3/vec4 round-trips in `test_worldserial` still pass — proves the extraction preserved behavior).

- [ ] **Step 4: Commit**
```
git -C C:/dev/personal/clang-examples add src/common/include/GlmJson.h src/common/include/ComponentSerialization.h
git -C C:/dev/personal/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "refactor(common): extract glm vec3/vec4 JSON helpers into ECS-free GlmJson.h"
```
Verify exactly those two files.

---

### Task 2: `EditorUI::ComboMapped` primitive

**Files:**
- Modify: `src/common/include/EditorUI.h`
- Modify: `src/editor/src/panels/inspector/EditorUIImpl.cpp`

ImGui layer — build-verified (no unit test, consistent with the rest of `EditorUIImpl`).

- [ ] **Step 1: Add the fn-ptr to the `EditorUI` struct (`src/common/include/EditorUI.h`)**

Add as the LAST member of the `EditorUI` struct (after the existing last member — read the file to confirm it; the existing last member is `bool (*Button)(const char* label);`):
```cpp
    // Edits an integer json value chosen from a fixed (label,value) set. Reads obj[key], selects the
    // index whose values[i] == obj[key] (default 0 if none match), shows a combo, and on change writes
    // values[selected] back. For enum-like ids whose stored value is NOT a 0..N-1 index (e.g. action ids).
    bool (*ComboMapped)(nlohmann::json& obj, const char* key,
                        const char* const* labels, const int* values, int count);
```

- [ ] **Step 2: Implement `UI_ComboMapped` (`src/editor/src/panels/inspector/EditorUIImpl.cpp`)**

In the anonymous namespace alongside `UI_Combo`, add:
```cpp
bool UI_ComboMapped(nlohmann::json& o, const char* k,
                    const char* const* labels, const int* values, int count) {
    if (!o.contains(k) || !o[k].is_number_integer()) return false;
    const int cur = o[k].get<int>();
    int idx = 0;
    for (int i = 0; i < count; ++i) if (values[i] == cur) { idx = i; break; }
    if (ImGui::Combo(k, &idx, labels, count)) { o[k] = values[idx]; return true; }
    return false;
}
```

- [ ] **Step 3: Wire it into `EditorUIInstance()` (`EditorUIImpl.cpp`)**

The `EditorUIInstance()` initializer lists the `UI_*` function pointers IN STRUCT ORDER. Add `&UI_ComboMapped` as the LAST entry of the initializer (matching its new last position in the struct). Read the existing initializer and append the pointer after the current last one (`&UI_Button`).

- [ ] **Step 4: Build — GREEN**
```
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
```
Expected: editor builds clean. (Member order in the struct MUST match the initializer order — if MSVC errors on the aggregate init, confirm `ComboMapped` is last in both.)

- [ ] **Step 5: Commit**
```
git -C C:/dev/personal/clang-examples add src/common/include/EditorUI.h src/editor/src/panels/inspector/EditorUIImpl.cpp
git -C C:/dev/personal/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(editor): EditorUI ComboMapped primitive (value-mapped combo for enum-like ids)"
```
Verify exactly those two files.

---

### Task 3: Move `MenuButtonComponent` + `Actions.h` to Game.dll (atomic migration)

**Files:**
- Create: `src/game/src/MenuButtonComponent.h`
- Move: `src/common/include/Actions.h` → `src/game/src/Actions.h`
- Modify: `src/common/include/ECS.h`
- Modify: `src/common/include/ComponentSerialization.h`
- Modify: `src/ecs/src/ComponentSerializers.cpp`
- Modify: `src/common/include/ECSCommands.h`
- Modify: `src/game/src/game.cpp`
- Delete: `src/editor/src/panels/inspector/MenuButtonEditor.cpp`, `.h`
- Modify: `src/editor/CMakeLists.txt`
- Modify: `src/editor/src/panels/EcsInspectorPanel.cpp`
- Modify: `tests/test_worldserial.cpp`

Atomic: the tree does not build until every edit is done. Make all edits, then build once. Locate edits by quoted code (line numbers are stale).

- [ ] **Step 1: Create `src/game/src/MenuButtonComponent.h`**
```cpp
#pragma once
#include <cstdint>
#include <glm/vec4.hpp>
#include <nlohmann/json.hpp>
#include "GlmJson.h"   // glm::vec4 JSON (adl_serializer) — ECS-free

// Marks a UI-rect entity as a clickable menu button (authored). ActionId is an Actions:: id
// (0 = none). The game interaction system drives UIRectComponent.Color between Normal/Hover/Press.
// Game-owned component: registered via GameRegisterComponents (non-builtin), not an engine builtin.
struct MenuButtonComponent {
    uint32_t  ActionId = 0;
    glm::vec4 Normal{0.15f, 0.15f, 0.18f, 1.0f};
    glm::vec4 Hover {0.25f, 0.25f, 0.30f, 1.0f};
    glm::vec4 Press {0.35f, 0.35f, 0.42f, 1.0f};
};

inline void to_json(nlohmann::json& j, const MenuButtonComponent& t) {
    j = nlohmann::json{
        {"ActionId", t.ActionId},
        {"Normal", t.Normal},
        {"Hover",  t.Hover},
        {"Press",  t.Press}
    };
}
inline void from_json(const nlohmann::json& j, MenuButtonComponent& t) {
    j.at("ActionId").get_to(t.ActionId);
    j.at("Normal").get_to(t.Normal);
    j.at("Hover").get_to(t.Hover);
    j.at("Press").get_to(t.Press);
}
```
(The `to_json`/`from_json` are copied verbatim from `ComponentSerialization.h`. `GlmJson.h` provides the `glm::vec4` `adl_serializer`, so `j = t.Normal` / `get_to` compile without `ECS.h`.)

- [ ] **Step 2: Move `Actions.h` into the game**
```
git -C C:/dev/personal/clang-examples mv src/common/include/Actions.h src/game/src/Actions.h
```
(`git mv` preserves history. The file content is unchanged — its only references are `game.cpp` and the about-to-be-deleted `MenuButtonEditor.cpp`.)

- [ ] **Step 3: Remove the struct + X-macro entry (`src/common/include/ECS.h`)**

Delete the `MenuButtonComponent` struct definition:
```cpp
// Marks a UI-rect entity as a clickable menu button (authored). ActionId is an Actions:: id
// (data binding to behavior; 0 = none). The interaction system drives UIRectComponent.Color
// between Normal/Hover/Press based on the pointer.
struct MenuButtonComponent {
    uint32_t  ActionId = 0;
    glm::vec4 Normal{0.15f, 0.15f, 0.18f, 1.0f};
    glm::vec4 Hover {0.25f, 0.25f, 0.30f, 1.0f};
    glm::vec4 Press {0.35f, 0.35f, 0.42f, 1.0f};
};
```
AND delete the X-macro line `    X(MenuButtonComponent) \` from `ECS_FOR_EACH_REGISTERED_COMPONENT` (keep the backslash-continuation chain valid: the line before still ends in ` \`, the line after still continues).

- [ ] **Step 4: Remove the (de)serializers (`src/common/include/ComponentSerialization.h`)**

Delete:
```cpp
inline void to_json(nlohmann::json& j, const MenuButtonComponent& t) {
    j = nlohmann::json{
        {"ActionId", t.ActionId},
        {"Normal", t.Normal},
        {"Hover",  t.Hover},
        {"Press",  t.Press}
    };
}
inline void from_json(const nlohmann::json& j, MenuButtonComponent& t) {
    j.at("ActionId").get_to(t.ActionId);
    j.at("Normal").get_to(t.Normal);
    j.at("Hover").get_to(t.Hover);
    j.at("Press").get_to(t.Press);
}
```

- [ ] **Step 5: Remove the builtin registration (`src/ecs/src/ComponentSerializers.cpp`)**

Delete the line:
```cpp
        r.Register<MenuButtonComponent>("MenuButtonComponent", true);
```

- [ ] **Step 6: Remove the typed branches (`src/common/include/ECSCommands.h`)**

In `ApplyComponentCommand`, delete:
```cpp
        } else if (componentData.Type == std::type_index(typeid(MenuButtonComponent))) {
            if (auto* btn = componentData.Get<MenuButtonComponent>()) {
                world.AddComponent(entity, *btn);
            }
```
Re-join the `else if` chain so the preceding branch chains directly into the following one (read the surrounding lines; no orphaned/missing brace).

In `RemoveComponentByType`, delete:
```cpp
        } else if (typeIndex == std::type_index(typeid(MenuButtonComponent))) {
            world.RemoveComponent<MenuButtonComponent>(entity);
```
Same care re-joining the chain.

- [ ] **Step 7: Include + register + hook (`src/game/src/game.cpp`)**

Update the existing `#include "Actions.h"` line — the path is unchanged (`"Actions.h"`) because `src/game/src` is on the game target's include path, but confirm it still resolves after the move (it will; the file now lives in `src/game/src`). Add after it:
```cpp
#include "MenuButtonComponent.h"  // game-owned component (moved out of ECS.h)
```
Confirm `#include "EditorUI.h"` is present (added during the Player migration; if absent, add it).

In `GameRegisterComponents()`, add after the existing `PlayerComponent` registration:
```cpp
    SerializerRegistry().Register<MenuButtonComponent>("MenuButtonComponent");
    SerializerRegistry().RegisterEditorHook("MenuButtonComponent", [](const EditorUI& ui, nlohmann::json& j) {
        static const char* names[] = { "None", "Play", "Quit", "Back" };
        static const int    ids[]  = { (int)Actions::None, (int)Actions::Play, (int)Actions::Quit, (int)Actions::Back };
        bool changed = false;
        changed |= ui.ComboMapped(j, "ActionId", names, ids, 4);
        changed |= ui.ColorEdit4(j, "Normal");
        changed |= ui.ColorEdit4(j, "Hover");
        changed |= ui.ColorEdit4(j, "Press");
        return changed;
    });
```

- [ ] **Step 8: Delete the dedicated editor**
```
git -C C:/dev/personal/clang-examples rm src/editor/src/panels/inspector/MenuButtonEditor.cpp src/editor/src/panels/inspector/MenuButtonEditor.h
```
In `src/editor/CMakeLists.txt`, delete the line `    src/panels/inspector/MenuButtonEditor.cpp`.
In `src/editor/src/panels/EcsInspectorPanel.cpp`, delete the include `#include "inspector/MenuButtonEditor.h"` AND the registration line `    m_Editors.push_back(std::make_unique<MenuButtonEditor>());`.

- [ ] **Step 9: Remove the orphaned serializer test (`tests/test_worldserial.cpp`)**

Delete the `T09_menubutton_roundtrip` function:
```cpp
static void T09_menubutton_roundtrip()
{
    MenuButtonComponent in;
    in.ActionId = 0x00010002u; // Nav/Quit
    in.Normal = glm::vec4(0.1f, 0.2f, 0.3f, 1.0f);
    in.Hover  = glm::vec4(0.4f, 0.5f, 0.6f, 1.0f);
    in.Press  = glm::vec4(0.7f, 0.8f, 0.9f, 1.0f);
    const nlohmann::json j = in;
    const auto out = j.get<MenuButtonComponent>();
    EXPECT(out.ActionId == in.ActionId);
    EXPECT(veq(glm::vec3(out.Normal), glm::vec3(in.Normal)));
    EXPECT(veq(glm::vec3(out.Hover),  glm::vec3(in.Hover)));
    EXPECT(veq(glm::vec3(out.Press),  glm::vec3(in.Press)));
}
```
AND remove its call `T09_menubutton_roundtrip();` from `main()`.

- [ ] **Step 10: Reconfigure + full build (green)**
```
cmake --preset msvc-win64-vs2026-enterprise
cmake --build --preset msvc-win64-vs2026-enterprise
```
Expected: all targets build clean. Any lingering `MenuButtonComponent` reference outside `src/game`, or a stale `Actions.h` include path, fails the build — fix it.

- [ ] **Step 11: Verify the type + Actions.h fully moved**
```
git -C C:/dev/personal/clang-examples grep -n "MenuButtonComponent" -- src/ecs src/engine src/editor src/common
git -C C:/dev/personal/clang-examples grep -n "Actions.h" -- src/common src/engine src/editor
```
Expected: the first returns NOTHING; the second returns NOTHING (no `src/common/include/Actions.h` remains; no engine/editor include of it). If anything remains, fix before committing.

- [ ] **Step 12: Commit**
```
git -C C:/dev/personal/clang-examples add -A
git -C C:/dev/personal/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(game): migrate MenuButtonComponent + Actions.h to Game.dll (ComboMapped action hook); remove typed branches + MenuButtonEditor"
```
Verify the commit contains: new `src/game/src/MenuButtonComponent.h`; renamed `Actions.h` (delete in common, add in game); modified `ECS.h`, `ComponentSerialization.h`, `ComponentSerializers.cpp`, `ECSCommands.h`, `game.cpp`, `editor/CMakeLists.txt`, `EcsInspectorPanel.cpp`, `test_worldserial.cpp`; deleted `MenuButtonEditor.{cpp,h}`. Nothing unrelated.

---

### Task 4: Full regression

**Files:** none (verification; commit fixups only if needed).

- [ ] **Step 1: Full clean build**
```
cmake --build --preset msvc-win64-vs2026-enterprise
```
Expected: all targets, no errors/`LNK`.

- [ ] **Step 2: Suites**
```
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_compserial.exe
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_reloadpreserve.exe
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_playermove.exe
```
Expected: each prints its pass line. (`test_compserial` emits the expected `NopeNotReal` warn; `test_reloadpreserve` the expected `BadComp` warn.)

- [ ] **Step 3: Manual smoke (optional/deferred, human-owned)**

After restarting the editor (required — `ECS.h` changed): with a menu-button entity —
1. **Inspector:** the `MenuButtonComponent` shows an **action dropdown** (None/Play/Quit/Back) and three RGBA color pickers; selecting an action and editing colors commits.
2. **Reload survival:** edit `src/game/src/Game.cpp` (`.cpp`-only), rebuild `game`; after hot-reload the entity keeps its `ActionId` + colors.
3. **Duplicate:** Ctrl+D the entity; the copy has `MenuButtonComponent` intact.
4. **Persistence + behavior:** save/reload `world.json` (round-trips); an existing world with a `"MenuButtonComponent"` entry still loads, and the menu still hovers/presses + fires its action.

- [ ] **Step 4: Commit fixups (only if needed).**

---

## Done criteria

- `GlmJson.h` exists (ECS-free); `ComponentSerialization.h` includes it; glm round-trips unchanged (Task 1).
- `EditorUI::ComboMapped` exists + implemented + wired in struct order (Task 2).
- `MenuButtonComponent` defined only in `src/game/src/MenuButtonComponent.h`; `git grep MenuButtonComponent` matches nothing under `src/ecs`/`src/engine`/`src/editor`/`src/common`; `Actions.h` lives in `src/game`, none in `src/common`; registered (non-builtin) + hooked with `ComboMapped` + 3× `ColorEdit4`; `MenuButtonEditor` deleted; typed branches removed (Task 3).
- Full tree builds; `test_compserial`/`test_reloadpreserve`/`test_ecs`/`test_worldserial`/`test_playermove` green (Task 4).
- No `GAME_API_VERSION` bump; full rebuild + editor restart documented.

## Notes

- Pattern follows the `PlayerComponent` migration; the two new surfaces are `GlmJson.h` (extraction, so a game header can serialize `glm::vec4` without `ECS.h`) and `EditorUI::ComboMapped` (reusable enum-like-id editor).
- `MenuButtonComponent` is POD → reload path is byte (memcpy); it keeps `to_json`/`from_json` for `world.json` disk persistence.
- Cold-start register-before-load ordering was already fixed in the Player migration (initial `LoadOrReload` precedes `LoadWorldSnapshot` on both GameThread and ServerApplication), so a persisted `MenuButtonComponent` registers before the world loads.
- The `editorDraw` hook is re-applied after `Register` on every reload (the `Register` upsert clears `editorDraw`); both run inside `GameRegisterComponents`.
```
