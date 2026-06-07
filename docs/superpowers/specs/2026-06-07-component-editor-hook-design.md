# Optional game-component editor hook — design

**Date:** 2026-06-07
**Branch:** `feat/component-editor-hook`
**Status:** DESIGN

## Goal

Let a game-defined component optionally provide its own custom editor rendering in the editor inspector, instead of the Piece-5 generic JSON-tree editor. `Game.dll` stays ImGui-free: the game draws through an engine-provided, ImGui-free function-pointer widget table (the `EditorUI` bridge, mirroring `NetServices`/`NavServices`) that operates on the component's JSON working copy. The hook is opt-in per component (falls back to the generic editor when absent) and is simply never invoked at runtime (no inspector there) — no macro gating.

## Context (as-built, verified)

- **ImGui is editor-only.** `imgui` is a STATIC lib (`third_party/CMakeLists.txt`) linked into `editor.exe` only (`src/editor/CMakeLists.txt:109`); `IMGUI_API` is empty (no DLL export). The editor creates/owns the single context (`ImGuiRenderer.cpp:194` `ImGui::CreateContext()`); panels draw between `NewFrame` (`:285`) and `Render` (`:530`); `m_EcsInspector.Draw(ctx)` at `:493`. `Game.dll` links no imgui (`src/game/CMakeLists.txt:48`) and includes no imgui.h.
- **Service-bridge pattern.** The game calls engine services via fn-ptr tables passed in `SystemContext` (`Systems.h`): `const NetServices* Net`, `const NavServices* Nav` (`NetServices.h`/`NavServices.h` are structs of function pointers the engine populates and the game calls — no linking). This is the template for `EditorUI`, except `EditorUI` is passed into the hook call by the editor (not via `SystemContext`).
- **Generic editor dispatch (Piece 5).** `EcsInspectorPanel.cpp:202-206` loops `SerializerRegistry().Entries()`, skips `builtin`/absent, and calls `m_GenericEditor.Draw(ctx, selectedEntity, en.name)`. `GenericComponentEditor::Draw` (`GenericComponentEditor.cpp:49-69`): re-syncs `m_Edit` (json) from the snapshot via `entry->save`, draws under a `CollapsingHeader(name)` via `DrawJsonValue`, and on change pushes `ECSCommand::ModifyComponentJson(entity, name, m_Edit.dump())`.
- **Serializer registry.** `ComponentSerializerEntry { std::string name; bool(*has); void(*save)(...,json&); void(*load)(...,const json&); void(*addDefault); void(*remove); bool builtin; }` (`ComponentSerializerRegistry.h:18-26`). `ComponentSerializerRegistry` (`Register<T>(name,builtin)`, `Entries()`, `Find(name)`); `ECS_API SerializerRegistry()` — single instance exported from `ecs.dll` (which links nlohmann, not imgui). `ComponentSerializerRegistry.h` already includes `<nlohmann/json.hpp>`.
- **Both editor + ecs.dll + game link nlohmann** (`nlohmann_json::nlohmann_json`), so a `nlohmann::json&` crossing the editor↔game hook call is the same type (same assumption Piece 4/5 already rely on).
- **Thread model.** The registry is a single shared instance; game registers on the GameThread (at startup, in `GameUpdate` seeding); the editor reads `Entries()` on the RenderThread. Pre-existing (Piece 4/5) — safe in practice because registration happens at startup before inspection; hot-reload re-register is the only race window (out of scope here).
- **vec3/vec4 json shape:** `{X,Y,Z}` / `{X,Y,Z,W}` or `{R,G,B,A}` (`ComponentSerialization.h`). The bridge's vector/color widgets handle this shape.

## Components

### 1. `EditorUI` bridge (new common header, ImGui-free)
`src/common/include/EditorUI.h` — a struct of function pointers, each operating on a `nlohmann::json& obj` by string `key`, returning `bool changed`. Initial widget set (sufficient for component fields; extensible later):
```cpp
#pragma once
#include <nlohmann/json.hpp>

// ImGui-free widget bridge: the editor implements these over Dear ImGui; a game component's
// editor hook calls them to draw/edit fields of a JSON working copy (keys = to_json field
// names). Each returns true if the value changed this frame. Missing/wrong-typed keys are a
// no-op returning false. Passed into the hook by the editor; Game.dll never links ImGui.
struct EditorUI {
    bool (*DragFloat) (nlohmann::json& obj, const char* key, float speed);
    bool (*DragFloat2)(nlohmann::json& obj, const char* key, float speed);
    bool (*DragFloat3)(nlohmann::json& obj, const char* key, float speed);
    bool (*DragFloat4)(nlohmann::json& obj, const char* key, float speed);
    bool (*InputInt)  (nlohmann::json& obj, const char* key);
    bool (*Checkbox)  (nlohmann::json& obj, const char* key);
    bool (*Combo)     (nlohmann::json& obj, const char* key, const char* const* labels, int count); // int field
    bool (*ColorEdit3)(nlohmann::json& obj, const char* key);  // {X,Y,Z} or {R,G,B}
    bool (*ColorEdit4)(nlohmann::json& obj, const char* key);  // {X,Y,Z,W} or {R,G,B,A}
    // Non-editing layout/output:
    void (*Text)      (const char* text);
    void (*Separator) ();
    void (*SameLine)  ();
    bool (*Button)    (const char* label);   // true on click (for game-defined inline actions)
};
```
- Float/vector widgets read the key as a number / `{X,Y,Z[,W]}` object, draw the ImGui control, write back, return changed. `DragFloat3`/`ColorEdit*` map the json object members. `Combo` edits an integer field by index.
- Robustness: a key absent from `obj`, or of an unexpected json type, is a safe no-op returning `false` (the editor checks `obj.contains(key)` + type before binding).

### 2. Editor-side implementation
`src/editor/src/panels/inspector/EditorUIImpl.{h,cpp}` — free functions implementing each `EditorUI` member over ImGui (operating on the passed `nlohmann::json&`), and a `const EditorUI& EditorUIInstance()` returning a `static const EditorUI` wired to them. Lives in the editor (links ImGui).

### 3. Registry hook field + registration
In `ComponentSerializerRegistry.h`:
- Forward-declare `struct EditorUI;` (a fn-ptr parameter only needs the type declared — no `EditorUI.h` include in `ecs.dll`).
- Add to `ComponentSerializerEntry`: `bool (*editorDraw)(const EditorUI&, nlohmann::json&) = nullptr;`
- Add `void RegisterEditorHook(const std::string& name, bool (*draw)(const EditorUI&, nlohmann::json&));` (non-template — keyed by the already-registered name): finds the entry by `name`, sets `editorDraw`; `SM_WARN` + no-op if the name isn't registered yet (the serializer `Register<T>` must come first). (A `template<class T> RegisterEditorHook` overload taking just the draw fn is optional sugar; the name-keyed form is enough.)
- Built-in `Register<T>` is unchanged; existing entries default `editorDraw = nullptr`.

### 4. Inspector dispatch (reuse `GenericComponentEditor`)
In `GenericComponentEditor::Draw`, inside the existing `CollapsingHeader(name)` block, choose the draw step:
```cpp
if (entry->editorDraw) {
    if (entry->editorDraw(EditorUIInstance(), m_Edit)) m_Modified = true;
} else {
    if (DrawJsonValue(name.c_str(), m_Edit)) m_Modified = true;
}
```
The snapshot→json sync above it and the `ModifyComponentJson` commit below it are unchanged. `EcsInspectorPanel`'s loop is unchanged (it already routes non-builtin components to `m_GenericEditor.Draw`).

### 5. Game usage (example; no engine/ecs edit needed to add a hook later)
After registering the serializer (Piece 4), the game registers an optional hook:
```cpp
SerializerRegistry().Register<MoveStats>("MoveStats");          // persistence (existing pattern)
SerializerRegistry().RegisterEditorHook("MoveStats",
    [](const EditorUI& ui, nlohmann::json& j) -> bool {
        bool changed = false;
        ui.Text("Movement");
        changed |= ui.DragFloat(j, "MoveSpeed", 0.1f);
        changed |= ui.Checkbox(j, "Flying");
        return changed;
    });
```
The hook fn is a captureless lambda (→ function pointer), compiled into `Game.dll`, never called at runtime (no inspector), and pulls in no imgui.

## Data flow

Editor inspector (RenderThread, inside the ImGui frame) → for a non-builtin component on the selected entity → `GenericComponentEditor::Draw` serializes it to `m_Edit` (json) via `entry->save` → if `entry->editorDraw` set, calls it with `EditorUIInstance()` + `m_Edit` (the hook, in Game.dll, draws via the bridge fn-ptrs which execute editor ImGui code on `m_Edit`) → on change, pushes `ModifyComponentJson` to the command ring → GameThread applies it via the registry `load` (existing Piece-5 path). 1–2 frame latency, same as all editor edits.

## Error handling

- Hook registered for an unknown name → `SM_WARN`, no-op (per logging-over-silent-skip).
- A widget key absent / wrong json type → no-op, returns false (no crash, no spurious edit).
- Hook fn-ptr is null (default) → generic editor used.
- Runtime (no inspector): hook never invoked.

## Testing

- **Unit (`test_compserial` or a new `test_editorhook`):** `RegisterEditorHook("PersistProbe", fn)` sets `Find("PersistProbe")->editorDraw == fn`; registering for an unknown name is a no-op (entry unchanged / null); a registered hook makes the dispatch predicate (`entry->editorDraw != nullptr`) true. (The `EditorUI` widgets + `GenericComponentEditor` ImGui dispatch are ImGui-coupled → not unit-testable; build-verified + reviewed.)
- **Manual GUI smoke (deferred / optional):** validating the actual rendered hook needs a real hooked game component on an entity (none exists yet). Optionally add a throwaway demo component + hook on a debug entity for a one-time visual check, then remove. Otherwise the visual path is exercised when a real hooked component lands.

## Out of scope (deliberate)

- Raw ImGui in `Game.dll` (shared-context approach) — rejected; keeps `Game.dll` ImGui-free.
- A compile-time macro to strip the hook from runtime builds — unnecessary (natural no-op + imgui-free bridge).
- Editing the raw typed struct (vs JSON) — the hook edits the component's JSON working copy by key, keeping the editor type-agnostic + the commit path unchanged.
- Hardening the registry's GameThread-write / RenderThread-read race (pre-existing Piece 4/5 concern; registration is at startup).
- Bespoke editors for built-in components (they keep their existing per-type `IComponentEditor`s).

## Decisions locked

- Fn-ptr `EditorUI` bridge (ImGui-free, json-by-key, returns changed); editor implements over ImGui; `Game.dll` stays ImGui-free.
- Optional `editorDraw` hook on `ComponentSerializerEntry` (forward-declared `EditorUI` in the registry header — no ecs.dll imgui/EditorUI.h dependency); set via `RegisterEditorHook(name, fn)`.
- Reuse `GenericComponentEditor::Draw` (hook-or-generic branch); snapshot-sync + `ModifyComponentJson` commit unchanged.
- No macro gating; hook is a no-op at runtime.
- Validated by a unit test for registration/selection; ImGui rendering build-verified + reviewed; manual visual smoke deferred to a real consumer.

## Build / test note

Build & test with the `msvc-win64-vs2026-community` preset only. Touches `src/common/include/EditorUI.h` (new) + `ComponentSerializerRegistry.h` (field + forward-decl + `RegisterEditorHook`), `src/editor/` (EditorUIImpl + the `GenericComponentEditor` branch + CMake), and a test. `ComponentSerializerRegistry.h` is consumed by ecs.dll/editor/game → rebuild `ecs` + `Engine` + editor + game. No `Game.dll` API change, no `GAME_API_VERSION` bump. Commit identity: `Nuno Silva <nuno.levezinho@live.com.pt>`.
