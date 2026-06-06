# Optional Game-Component Editor Hook Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a game-defined component optionally supply its own editor rendering (via an ImGui-free `EditorUI` fn-ptr bridge), falling back to the Piece-5 generic JSON editor; `Game.dll` stays ImGui-free and the hook is simply never called at runtime.

**Architecture:** Add an ImGui-free `EditorUI` widget bridge (common header) that operates on a component's `nlohmann::json` working copy by key. Add an optional `editorDraw` fn-ptr to the serializer registry entry, set via `RegisterEditorHook(name, fn)`. The editor implements `EditorUI` over ImGui and, in `GenericComponentEditor::Draw`, calls the hook when present (else the generic tree); commit stays `ModifyComponentJson`.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-community`), CMake, Dear ImGui (editor only), nlohmann/json. Builds on Piece 4 (serializer registry) + Piece 5 (generic editor).

**Scope:** Implements `docs/superpowers/specs/2026-06-07-component-editor-hook-design.md`. No raw ImGui in `Game.dll`, no macro gating, no `GAME_API_VERSION` bump. Touches `ComponentSerializerRegistry.h` (ecs.dll/common header) → rebuild ecs + Engine + editor + game.

---

## Background facts (verified)

- `ComponentSerializerEntry` (`ComponentSerializerRegistry.h:18-26`) aggregate has 7 members ending `bool builtin;`. `Register<T>` (`:33-48`) brace-inits exactly those 7 in order. The registry singleton `SerializerRegistry()` is exported from ecs.dll; `RegisterEditorHook` will be a member declared in the header, **defined in `src/ecs/src/ComponentSerializers.cpp`** (which already includes `lib.h` for `SM_WARN`). The header includes `<nlohmann/json.hpp>` already; it does NOT include `lib.h` (keep it that way).
- `GenericComponentEditor::Draw` (`src/editor/src/panels/inspector/GenericComponentEditor.cpp:49-69`): gets `entry = SerializerRegistry().Find(name)`, re-syncs `m_Edit` (json) via `entry->save`, then:
  ```cpp
  if (ImGui::CollapsingHeader(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
      if (DrawJsonValue(name.c_str(), m_Edit)) m_Modified = true;
      if (m_Modified) {
          if (!ctx.App->ECSCommandRing.Push(ECSCommand::ModifyComponentJson(entity, name, m_Edit.dump())))
              SM_WARN("ECS command queue full! ModifyComponentJson dropped.");
          m_Modified = false;
      }
  }
  ```
- `EcsInspectorPanel.cpp:202-206` already routes non-builtin components to `m_GenericEditor.Draw(ctx, selectedEntity, en.name)` — unchanged by this work.
- Editor links `imgui` (`src/editor/CMakeLists.txt:109`) + lists inspector sources in `add_executable(editor ...)` (where `src/panels/inspector/GenericComponentEditor.cpp` was added for Piece 5). Game/ecs link no imgui.
- vec4 json shape is `{X,Y,Z,W}` OR `{R,G,B,A}` (`ComponentSerialization.h`); vec3 `{X,Y,Z}`. ImGui version 1.92.x (`ImGui::DragFloat/DragFloat3/Checkbox/InputInt/Combo/ColorEdit3/ColorEdit4/Button/Text/Separator/SameLine` all available).
- `test_compserial.cpp` already includes `ComponentSerializerRegistry.h` + a synthetic `PersistProbe` + the `EXPECT` harness + a `platform_debug_break` stub; links ecs + nlohmann. Reuse it.

## Type/symbol contract (keep exact)

- New `src/common/include/EditorUI.h`: `struct EditorUI { ... fn-ptrs ... };` (ImGui-free; depends only on `<nlohmann/json.hpp>`).
- `ComponentSerializerRegistry.h`: forward-declare `struct EditorUI;`; add LAST member to `ComponentSerializerEntry`: `bool (*editorDraw)(const EditorUI&, nlohmann::json&) = nullptr;`; add member `void RegisterEditorHook(const std::string& name, bool (*draw)(const EditorUI&, nlohmann::json&));` (declared here, defined in ComponentSerializers.cpp).
- New `src/editor/src/panels/inspector/EditorUIImpl.{h,cpp}`: `const EditorUI& EditorUIInstance();`.

---

### Task 1: `EditorUI` bridge type + registry hook field + `RegisterEditorHook` + unit test

**Files:**
- Create: `src/common/include/EditorUI.h`
- Modify: `src/common/include/ComponentSerializerRegistry.h`
- Modify: `src/ecs/src/ComponentSerializers.cpp`
- Modify: `tests/test_compserial.cpp`

- [ ] **Step 1: Write the failing test (extend `tests/test_compserial.cpp`)**

Add `#include "EditorUI.h"` at the top. Add a stub hook + test, registered in `main()` after the existing tests:
```cpp
// A stub editor hook (no ImGui — unit tests can't run an ImGui frame). Just a function
// pointer the registry should store + expose.
static bool StubEditorHook(const EditorUI&, nlohmann::json&) { return false; }

static void T07_register_editor_hook()
{
    SerializerRegistry().Register<PersistProbe>("PersistProbe"); // ensure entry exists
    // Default: no hook.
    EXPECT(SerializerRegistry().Find("PersistProbe") != nullptr);
    EXPECT(SerializerRegistry().Find("PersistProbe")->editorDraw == nullptr);

    SerializerRegistry().RegisterEditorHook("PersistProbe", &StubEditorHook);
    EXPECT(SerializerRegistry().Find("PersistProbe")->editorDraw == &StubEditorHook);

    // Unknown name: no-op, no crash.
    SerializerRegistry().RegisterEditorHook("NopeNotReal", &StubEditorHook);
    EXPECT(SerializerRegistry().Find("NopeNotReal") == nullptr);

    // Built-ins default to no hook.
    EXPECT(SerializerRegistry().Find("TransformComponent")->editorDraw == nullptr);
}
```
Register `T07_register_editor_hook();` in `main()`.

- [ ] **Step 2: Build — expect RED**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_compserial
```
Expected: errors — `EditorUI.h` not found / `editorDraw` not a member / `RegisterEditorHook` not a member.

- [ ] **Step 3: Create `src/common/include/EditorUI.h`**
```cpp
#pragma once
#include <nlohmann/json.hpp>

// ImGui-free widget bridge: the editor implements these over Dear ImGui; a game component's
// editor hook (in Game.dll, which links no ImGui) calls them to draw/edit fields of a JSON
// working copy. Keys are the component's to_json field names. Each editing widget returns true
// if the value changed this frame; a missing/wrong-typed key is a safe no-op returning false.
// The editor passes an EditorUI into the hook; Game.dll only calls through the pointers.
struct EditorUI {
    bool (*DragFloat) (nlohmann::json& obj, const char* key, float speed);
    bool (*DragFloat2)(nlohmann::json& obj, const char* key, float speed);
    bool (*DragFloat3)(nlohmann::json& obj, const char* key, float speed);
    bool (*DragFloat4)(nlohmann::json& obj, const char* key, float speed);
    bool (*InputInt)  (nlohmann::json& obj, const char* key);
    bool (*Checkbox)  (nlohmann::json& obj, const char* key);
    bool (*Combo)     (nlohmann::json& obj, const char* key, const char* const* labels, int count);
    bool (*ColorEdit3)(nlohmann::json& obj, const char* key);
    bool (*ColorEdit4)(nlohmann::json& obj, const char* key);
    void (*Text)      (const char* text);
    void (*Separator) ();
    void (*SameLine)  ();
    bool (*Button)    (const char* label);
};
```

- [ ] **Step 4: Registry changes (`ComponentSerializerRegistry.h`)**

- Near the top (after includes, before `ComponentSerializerEntry`), forward-declare:
```cpp
struct EditorUI; // defined in EditorUI.h (editor implements it over ImGui); only a fn-ptr param here
```
- Add as the LAST member of `ComponentSerializerEntry` (after `bool builtin;`):
```cpp
    // Optional custom editor renderer (game-provided, ImGui-free via the EditorUI bridge).
    // Null => the inspector uses the generic JSON-tree editor. Set via RegisterEditorHook.
    bool (*editorDraw)(const EditorUI&, nlohmann::json&) = nullptr;
```
  (The existing `Register<T>` 7-element brace-init leaves this defaulted to `nullptr` — do NOT add it to that brace list.)
- Add a member declaration to `ComponentSerializerRegistry` (after `Find`):
```cpp
    // Attach an optional custom editor renderer to an already-registered component (by name).
    // No-op (warns) if the name isn't registered. Defined in ComponentSerializers.cpp.
    void RegisterEditorHook(const std::string& name, bool (*draw)(const EditorUI&, nlohmann::json&));
```

- [ ] **Step 5: Define `RegisterEditorHook` (`src/ecs/src/ComponentSerializers.cpp`)**

`m_Entries` is private to `ComponentSerializerRegistry`; the method is a member, so it has access. Add the definition (the file already includes `ComponentSerializerRegistry.h` + `lib.h`):
```cpp
void ComponentSerializerRegistry::RegisterEditorHook(const std::string& name,
                                                     bool (*draw)(const EditorUI&, nlohmann::json&)) {
    for (auto& e : m_Entries) { if (e.name == name) { e.editorDraw = draw; return; } }
    SM_WARN("RegisterEditorHook: no serializer registered for '%s' — hook ignored", name.c_str());
}
```
(`m_Entries` is private; if the compiler rejects accessing it from a non-inline member defined out-of-class, that's not the case — out-of-class member definitions have full private access. No `friend` needed.)

- [ ] **Step 6: Build + run — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community --target ecs --target test_compserial
./out/build/msvc-win64-vs2026-community/bin/Debug/test_compserial.exe
```
Expected: `All component-serializer tests passed.` Watch for: the aggregate-init in `Register<T>` must still compile (editorDraw defaulted) — if MSVC complains about the brace count, confirm `editorDraw` is the LAST member with a default initializer.

- [ ] **Step 7: Commit**
```
git -C /c/dev/clang-examples add src/common/include/EditorUI.h src/common/include/ComponentSerializerRegistry.h src/ecs/src/ComponentSerializers.cpp tests/test_compserial.cpp
git -C /c/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(editor): EditorUI bridge type + registry editorDraw hook + RegisterEditorHook"
```
Verify exactly those four files.

---

### Task 2: Editor-side `EditorUI` impl + inspector dispatch

**Files:**
- Create: `src/editor/src/panels/inspector/EditorUIImpl.h`
- Create: `src/editor/src/panels/inspector/EditorUIImpl.cpp`
- Modify: `src/editor/src/panels/inspector/GenericComponentEditor.cpp`
- Modify: `src/editor/CMakeLists.txt`

ImGui UI — build-verified + reviewed (no unit test; the registration is covered by Task 1).

- [ ] **Step 1: Create `EditorUIImpl.h`**
```cpp
#pragma once
#include "EditorUI.h"

// The editor's ImGui-backed implementation of the EditorUI bridge. Returns a process-wide
// static table game component editor hooks call through.
const EditorUI& EditorUIInstance();
```

- [ ] **Step 2: Create `EditorUIImpl.cpp`**

Implement each widget over ImGui + the json. Pattern (apply consistently; full set below):
```cpp
#include "EditorUIImpl.h"
#include <imgui.h>
#include <array>

namespace {

bool UI_DragFloat(nlohmann::json& o, const char* k, float speed) {
    if (!o.contains(k) || !o[k].is_number()) return false;
    float v = o[k].get<float>();
    if (ImGui::DragFloat(k, &v, speed)) { o[k] = v; return true; }
    return false;
}

// Reads N float members from a json OBJECT under `k` using the given member keys (e.g. X,Y,Z),
// draws a DragFloatN, writes back. Returns changed. No-op if shape mismatches.
template <int N>
bool DragVecN(nlohmann::json& o, const char* k, const char* const (&members)[N], float speed) {
    if (!o.contains(k) || !o[k].is_object()) return false;
    float v[N];
    for (int i = 0; i < N; ++i) {
        if (!o[k].contains(members[i]) || !o[k][members[i]].is_number()) return false;
        v[i] = o[k][members[i]].get<float>();
    }
    bool changed = false;
    if      constexpr (N == 2) changed = ImGui::DragFloat2(k, v, speed);
    else if constexpr (N == 3) changed = ImGui::DragFloat3(k, v, speed);
    else if constexpr (N == 4) changed = ImGui::DragFloat4(k, v, speed);
    if (changed) for (int i = 0; i < N; ++i) o[k][members[i]] = v[i];
    return changed;
}
bool UI_DragFloat2(nlohmann::json& o, const char* k, float s){ static const char* m[2]={"X","Y"};       return DragVecN(o,k,m,s); }
bool UI_DragFloat3(nlohmann::json& o, const char* k, float s){ static const char* m[3]={"X","Y","Z"};   return DragVecN(o,k,m,s); }
bool UI_DragFloat4(nlohmann::json& o, const char* k, float s){ static const char* m[4]={"X","Y","Z","W"};return DragVecN(o,k,m,s); }

bool UI_InputInt(nlohmann::json& o, const char* k) {
    if (!o.contains(k) || !o[k].is_number_integer()) return false;
    int v = o[k].get<int>();
    if (ImGui::InputInt(k, &v)) { o[k] = v; return true; }
    return false;
}
bool UI_Checkbox(nlohmann::json& o, const char* k) {
    if (!o.contains(k) || !o[k].is_boolean()) return false;
    bool v = o[k].get<bool>();
    if (ImGui::Checkbox(k, &v)) { o[k] = v; return true; }
    return false;
}
bool UI_Combo(nlohmann::json& o, const char* k, const char* const* labels, int count) {
    if (!o.contains(k) || !o[k].is_number_integer()) return false;
    int v = o[k].get<int>();
    if (v < 0 || v >= count) return false;
    if (ImGui::Combo(k, &v, labels, count)) { o[k] = v; return true; }
    return false;
}
// Color: try {R,G,B[,A]} then {X,Y,Z[,W]} member sets.
template <int N>
bool ColorN(nlohmann::json& o, const char* k) {
    if (!o.contains(k) || !o[k].is_object()) return false;
    static const char* rgba[4] = {"R","G","B","A"};
    static const char* xyzw[4] = {"X","Y","Z","W"};
    const char* const* m = (o[k].contains("R")) ? rgba : xyzw;
    float v[N];
    for (int i = 0; i < N; ++i) {
        if (!o[k].contains(m[i]) || !o[k][m[i]].is_number()) return false;
        v[i] = o[k][m[i]].get<float>();
    }
    bool changed = (N == 4) ? ImGui::ColorEdit4(k, v) : ImGui::ColorEdit3(k, v);
    if (changed) for (int i = 0; i < N; ++i) o[k][m[i]] = v[i];
    return changed;
}
bool UI_ColorEdit3(nlohmann::json& o, const char* k){ return ColorN<3>(o,k); }
bool UI_ColorEdit4(nlohmann::json& o, const char* k){ return ColorN<4>(o,k); }

void UI_Text(const char* t){ ImGui::TextUnformatted(t); }
void UI_Separator(){ ImGui::Separator(); }
void UI_SameLine(){ ImGui::SameLine(); }
bool UI_Button(const char* l){ return ImGui::Button(l); }

} // namespace

const EditorUI& EditorUIInstance() {
    static const EditorUI ui{
        &UI_DragFloat, &UI_DragFloat2, &UI_DragFloat3, &UI_DragFloat4,
        &UI_InputInt, &UI_Checkbox, &UI_Combo, &UI_ColorEdit3, &UI_ColorEdit4,
        &UI_Text, &UI_Separator, &UI_SameLine, &UI_Button
    };
    return ui;
}
```
(Member-init order of the `EditorUI` aggregate MUST match the struct declaration order in `EditorUI.h`.)

- [ ] **Step 3: Inspector dispatch (`GenericComponentEditor.cpp`)**

Add includes: `#include "EditorUIImpl.h"`. In `Draw`, replace the draw step inside the `CollapsingHeader` block:
```cpp
    if (ImGui::CollapsingHeader(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (entry->editorDraw) {
            if (entry->editorDraw(EditorUIInstance(), m_Edit)) m_Modified = true;
        } else {
            if (DrawJsonValue(name.c_str(), m_Edit)) m_Modified = true;
        }
        if (m_Modified) {
            if (!ctx.App->ECSCommandRing.Push(ECSCommand::ModifyComponentJson(entity, name, m_Edit.dump())))
                SM_WARN("ECS command queue full! ModifyComponentJson dropped.");
            m_Modified = false;
        }
    }
```
(`entry` is the `SerializerRegistry().Find(name)` already obtained at the top of `Draw`. The snapshot→`m_Edit` sync above + the commit are unchanged.)

- [ ] **Step 4: `src/editor/CMakeLists.txt`** — add `src/panels/inspector/EditorUIImpl.cpp` to the `add_executable(editor ...)` source list, alongside `GenericComponentEditor.cpp`.

- [ ] **Step 5: Full build (green)**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community
```
Expected: all targets incl `editor` build clean. (Reconfigure for the new source.)

- [ ] **Step 6: Commit**
```
git -C /c/dev/clang-examples add src/editor/src/panels/inspector/EditorUIImpl.h src/editor/src/panels/inspector/EditorUIImpl.cpp src/editor/src/panels/inspector/GenericComponentEditor.cpp src/editor/CMakeLists.txt
git -C /c/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(editor): ImGui EditorUI impl + inspector calls component editorDraw hook (else generic)"
```
Verify exactly those four files.

---

### Task 3: Full regression

**Files:** none (verification; commit fixups only if needed).

- [ ] **Step 1: Full clean build** — `cmake --build --preset msvc-win64-vs2026-community`. Expect all targets, no errors/`LNK`.

- [ ] **Step 2: Suites**
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_compserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
```
Expected: each prints its pass line.

- [ ] **Step 3: Manual smoke (optional/deferred, human-owned)**

No game component registers an `editorDraw` hook yet, so the generic editor still drives all game components (no visible change). To visually verify the hook path, a dev can temporarily register a serializer + `RegisterEditorHook` for a throwaway component on an entity and confirm the custom widgets render + edits commit, then remove it. Otherwise the visual path is exercised when a real hooked component lands. The existing generic-editor behavior must be unchanged for components without a hook (confirm in the editor: built-in + unhooked game components edit as before).

- [ ] **Step 4: Commit fixups (only if needed).**

---

## Done criteria

- `EditorUI` bridge type exists (common, ImGui-free); `ComponentSerializerEntry` has an optional `editorDraw`; `RegisterEditorHook(name, fn)` sets it (no-op+warn on unknown name) — unit-tested (Task 1).
- The editor implements `EditorUI` over ImGui; `GenericComponentEditor::Draw` calls `editorDraw` when present, else the generic tree; commit unchanged (Task 2).
- Full tree builds; `test_compserial`/`test_ecs`/`test_worldserial` green; unhooked components unchanged (Task 3).
- A game can add a custom component editor with `RegisterEditorHook("Name", [](const EditorUI& ui, json& j){ ... })` — no engine/ecs edit, no imgui in `Game.dll`, no-op at runtime.

## Notes

- The hook edits the component's **json by key** (keys = `to_json` field names) and reuses the Piece-5 `ModifyComponentJson` commit — the editor stays type-agnostic.
- Pre-existing (Piece 4/5) registry GameThread-write / RenderThread-read race is unchanged; registration is at game startup. Out of scope.
- Widget set is the initial vocabulary; extend `EditorUI` + `EditorUIImpl` as real hooks need more (each addition is editor + header only, not a `Game.dll` ABI change beyond the new fn-ptr).
