# Animator Node-Graph Editor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A visual node-graph editor for `AnimatorController` assets — states as nodes, transitions as links — that saves back to `.animctrl.json` and live-reloads running entities, with a live active-state highlight.

**Architecture:** Vendor `thedmd/imgui-node-editor` (editor-only). New `AnimatorGraphPanel` (top-level, owned by `ImGuiRenderer`) edits a mutable working copy of a controller. The engine `AnimatorControllerStore` becomes `shared_ptr<const>`-backed with a `Reload` swap + source-path tracking so Save writes JSON and live-reloads without tearing the GameThread evaluator's read. Layout (node positions) lives in an `editorLayout` block in the controller JSON.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-community` preset ONLY), CMake, ImGui + imgui-node-editor, nlohmann::json, NVRHI.

**Spec:** `docs/superpowers/specs/2026-06-08-anim-graph-editor-design.md`

---

## Conventions (apply to every task)

- **Build/test preset:** `msvc-win64-vs2026-community` ONLY (enterprise not installed). Binaries: `out/build/msvc-win64-vs2026-community/bin/Debug/`.
- **Commit author:** EVERY commit passes `--author="Nuno Silva <nuno.levezinho@live.com.pt>"`. NEVER the vinci-energies.net work email.
- **NEVER** `--no-verify`. **NEVER** `git add -A` / `git add .` — stage exact paths only. Never stage `assets/world.json`, `assets/engine_settings.json`, `assets/editor_preferences.json` (gitignored local files).
- **git from git-bash:** cwd doesn't persist — use `git -C C:/dev/clang-examples ...`.
- Branch is already `feat/anim-graph-editor`.
- **Logging:** `SM_TRACE`/`SM_WARN`/`SM_ERROR` (printf-style args work), never `printf`/`cout`. Log on degradation paths, never silent-skip.
- New source files must be added to the owning `CMakeLists.txt` explicitly (no globbing): third_party libs in `third_party/CMakeLists.txt`; editor sources in `src/editor/CMakeLists.txt`; engine in `src/engine/CMakeLists.txt`; tests in `tests/CMakeLists.txt`.
- **No `ECS.h` change in this feature** → no editor restart needed for hot-reload of `game`; but `Engine.dll`/`editor.exe` rebuilds DO require relaunching the editor to test (this is a normal relaunch, not the ECS-layout restart caveat).

---

## File Structure

**Create:**
- `third_party/imgui-node-editor/` — vendored library source + headers.
- `src/editor/src/panels/AnimatorGraphPanel.h` / `.cpp` — the node-graph panel (window, controller picker, canvas render, editing, save, live highlight).
- `src/editor/src/panels/AnimatorGraphLayout.h` — pure helpers for the `editorLayout` JSON block (read/write node positions to/from raw json), header-only so it's testable without ImGui.
- `tests/test_animgraph.cpp` — pure-fn tests (to_json round-trip, RenameState, ValidateController, editorLayout round-trip, store Reload).

**Modify:**
- `third_party/CMakeLists.txt` — add the `imgui-node-editor` static lib target.
- `src/common/include/AnimatorController.h` — add `to_json(AnimatorController)`, pure helpers `RenameState`, `ValidateController`.
- `src/engine/src/animation/AnimatorControllerStore.{h,cpp}` — `shared_ptr<const>` entries, `Get` returns shared_ptr, new `Reload` + source-path (`Add` gains a path param, `SourcePathForHandle`).
- `src/engine/src/threading/GameThread.cpp` — evaluator `Get` decl change (ptr → shared_ptr) + drain `Add` call passes the source path.
- `src/editor/src/panels/inspector/AnimatorEditor.cpp` — `Get` decl change (ptr → shared_ptr) at the readout site.
- `src/editor/src/app/ImGuiRenderer.{h,cpp}` — own + draw `AnimatorGraphPanel`.
- `src/editor/CMakeLists.txt` — add `AnimatorGraphPanel.cpp`; link `imgui-node-editor`.
- `tests/CMakeLists.txt` — add `test_animgraph`.

---

## Task 1: Vendor imgui-node-editor

> HIGH-RISK task: pulls an external dependency + CMake integration + must compile against the repo's bundled ImGui. Do this carefully and STOP/escalate if the library won't compile against the project's ImGui version.

**Files:**
- Create: `third_party/imgui-node-editor/` (vendored source)
- Modify: `third_party/CMakeLists.txt`, `src/editor/CMakeLists.txt`

- [ ] **Step 1: Fetch the library source at a pinned tag**

Clone into a temp dir and copy the source files (do NOT keep the whole repo / its examples / .git). From a shell:
```
git clone --depth 1 --branch v0.9.3 https://github.com/thedmd/imgui-node-editor.git /tmp/ine
```
If `v0.9.3` is unavailable, list tags (`git ls-remote --tags https://github.com/thedmd/imgui-node-editor.git`) and pick the latest stable `v0.9.x`. Record the exact tag used in the commit message.

Copy ONLY these files (the library core — NOT the `examples/`, `misc/`, images, or docs) into `third_party/imgui-node-editor/`:
```
imgui_node_editor.h
imgui_node_editor.cpp
imgui_node_editor_api.cpp
imgui_node_editor_internal.h
imgui_node_editor_internal.inl
imgui_canvas.h
imgui_canvas.cpp
imgui_bezier_math.h
imgui_bezier_math.inl
imgui_extra_math.h
imgui_extra_math.inl
crude_json.h
crude_json.cpp
LICENSE
```
(Verify the exact file set against the cloned repo root — the list above is the v0.9.x core. If a referenced `.inl`/`.h` is missing or differently named, copy what the `.cpp`s actually `#include`. The goal: the 4 `.cpp` translation units + every header/inl they include, nothing else.)

- [ ] **Step 2: Add the CMake static lib (mirror ImGuizmo)**

In `third_party/CMakeLists.txt`, after the `ImGuizmo` block (search `add_library(ImGuizmo`), add:
```cmake
add_library(imgui-node-editor STATIC
    imgui-node-editor/imgui_node_editor.cpp
    imgui-node-editor/imgui_node_editor_api.cpp
    imgui-node-editor/imgui_canvas.cpp
    imgui-node-editor/crude_json.cpp
)
set_target_properties(imgui-node-editor PROPERTIES
    OUTPUT_NAME imgui-node-editor
    FOLDER ThirdParty/imgui-node-editor
)
target_include_directories(imgui-node-editor PUBLIC imgui-node-editor)
target_link_libraries(imgui-node-editor PRIVATE imgui)
```
(Match the exact property style of the neighboring `ImGuizmo` block — copy its `set_target_properties` shape, e.g. any `ARCHIVE_OUTPUT_DIRECTORY`/folder conventions it uses.)

- [ ] **Step 3: Link it into the editor**

In `src/editor/CMakeLists.txt`, in the `target_link_libraries(editor PRIVATE ...)` list, add `imgui-node-editor` directly after the `ImGuizmo` entry.

- [ ] **Step 4: Compile-smoke the library into the editor**

Add a temporary include to confirm it compiles + links against the project's ImGui. In `src/editor/src/app/ImGuiRenderer.cpp`, near the other includes, TEMPORARILY add `#include <imgui_node_editor.h>` and build:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: `editor.exe` builds + links clean (the node-editor lib compiles against the bundled ImGui). If it fails on ImGui API mismatch, the pinned tag is incompatible with the repo's ImGui — try the next-older `v0.9.x` tag, or report BLOCKED with the exact compile error.

After a clean build, REMOVE the temporary include (Task 4 adds the real include in the panel). Rebuild editor once more to confirm it's still clean without the temp include.

- [ ] **Step 5: Commit**
```
git -C C:/dev/clang-examples add third_party/imgui-node-editor third_party/CMakeLists.txt src/editor/CMakeLists.txt
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "build(anim): vendor thedmd/imgui-node-editor <tag> (editor-only) for the animator graph editor"
```
(Replace `<tag>` with the exact tag used. Stage the WHOLE `third_party/imgui-node-editor` dir — it's vendored source, intended to be committed; verify none of its files are gitignored with `git -C C:/dev/clang-examples status --short third_party/imgui-node-editor`.)

---

## Task 2: Engine seam — store shared_ptr + Reload + source path

**Files:**
- Modify: `src/engine/src/animation/AnimatorControllerStore.h`, `.cpp`
- Modify: `src/engine/src/threading/GameThread.cpp`
- Modify: `src/editor/src/panels/inspector/AnimatorEditor.cpp`

- [ ] **Step 1: Rewrite the store header**

Replace `src/engine/src/animation/AnimatorControllerStore.h` with:
```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <memory>
#include <mutex>
#include "Engine.h"          // ENGINE_API
#include "AnimatorController.h"

// Process-wide store of AnimatorController assets, keyed by a stable hash handle (AssetKeyHash of
// "<modelKey>#animctrl"). Entries are shared_ptr<const AnimatorController>: the GameThread evaluator
// loads the shared_ptr once per tick (its own ref keeps the graph alive for the tick), so the editor
// can atomically Reload (swap) an entry without tearing the read. Mutex-guarded. Mirrors the engine's
// shared_ptr<const ECS> snapshot pattern.
class ENGINE_API AnimatorControllerStore {
public:
    static AnimatorControllerStore& Instance();
    // First load: de-dup by key (no-op if already present). `sourcePath` = the .animctrl.json the
    // controller was loaded from (so the editor's Save writes back to the same file).
    uint64_t Add(const std::string& key, AnimatorController controller, const std::string& sourcePath = "");
    // Editor write-back: ALWAYS replaces the entry's controller (atomic shared_ptr swap under mutex).
    void     Reload(const std::string& key, AnimatorController controller);
    std::shared_ptr<const AnimatorController> Get(uint64_t handle) const;   // null if unknown
    std::string KeyForHandle(uint64_t handle) const;
    std::string SourcePathForHandle(uint64_t handle) const;                 // "" if unknown/unset
    std::vector<std::pair<uint64_t, std::string>> GetAssetList() const;
private:
    AnimatorControllerStore() = default;
    struct Entry { std::string key; std::shared_ptr<const AnimatorController> controller; std::string sourcePath; };
    mutable std::mutex m_Mutex;
    std::unordered_map<uint64_t, Entry> m_ByHandle;
};
```

- [ ] **Step 2: Rewrite the store impl**

Replace `src/engine/src/animation/AnimatorControllerStore.cpp` with:
```cpp
#include "animation/AnimatorControllerStore.h"
#include "AssetKey.h"

AnimatorControllerStore& AnimatorControllerStore::Instance() { static AnimatorControllerStore s; return s; }

uint64_t AnimatorControllerStore::Add(const std::string& key, AnimatorController controller, const std::string& sourcePath) {
    const uint64_t handle = AssetKeyHash(key);
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    if (it != m_ByHandle.end()) return handle;   // de-dup: first-load only
    m_ByHandle.emplace(handle, Entry{ key, std::make_shared<const AnimatorController>(std::move(controller)), sourcePath });
    return handle;
}
void AnimatorControllerStore::Reload(const std::string& key, AnimatorController controller) {
    const uint64_t handle = AssetKeyHash(key);
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    if (it == m_ByHandle.end()) {                // not present yet -> treat as first add (preserve no path)
        m_ByHandle.emplace(handle, Entry{ key, std::make_shared<const AnimatorController>(std::move(controller)), std::string() });
        return;
    }
    it->second.controller = std::make_shared<const AnimatorController>(std::move(controller)); // atomic swap of the ptr; old refs survive
}
std::shared_ptr<const AnimatorController> AnimatorControllerStore::Get(uint64_t handle) const {
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    return it == m_ByHandle.end() ? nullptr : it->second.controller;
}
std::string AnimatorControllerStore::KeyForHandle(uint64_t handle) const {
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    return it == m_ByHandle.end() ? std::string() : it->second.key;
}
std::string AnimatorControllerStore::SourcePathForHandle(uint64_t handle) const {
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    return it == m_ByHandle.end() ? std::string() : it->second.sourcePath;
}
std::vector<std::pair<uint64_t, std::string>> AnimatorControllerStore::GetAssetList() const {
    std::scoped_lock lk(m_Mutex);
    std::vector<std::pair<uint64_t, std::string>> out;
    out.reserve(m_ByHandle.size());
    for (const auto& [h, e] : m_ByHandle) out.emplace_back(h, e.key);
    return out;
}
```

- [ ] **Step 3: Update the evaluator + drain in GameThread.cpp**

The evaluator: find `const AnimatorController* ctrl = ... AnimatorControllerStore::Instance().Get(animator->ControllerId) : nullptr;` (in `PublishPaletteFrame`). Change the declared type from `const AnimatorController*` to `std::shared_ptr<const AnimatorController>`:
```cpp
        std::shared_ptr<const AnimatorController> ctrl =
            (animator && animator->ControllerId) ? AnimatorControllerStore::Instance().Get(animator->ControllerId) : nullptr;
```
The subsequent `if (ctrl)` and `EvaluateAnimator(*sk, *ctrl, a, dt)` work unchanged (`*ctrl` derefs the shared_ptr; the ref is held for the tick). Confirm no other line stores the raw pointer.

The drain: find `AnimatorControllerStore::Instance().Add(res.assetKey + "#animctrl", std::move(ctrl));`. Pass the source path. The worker parsed the controller from `job.objPath`'s sibling; carry that path through `ModelLoadResult`. Add a field `std::string controllerSourcePath;` to `ModelLoadResult` (in `GameThread.h`), set it in the worker right where `result.hasController = true;` is set:
```cpp
                    result.controllerSourcePath = ctrlPath.string();
```
and pass it in the drain:
```cpp
                        AnimatorControllerStore::Instance().Add(res.assetKey + "#animctrl", std::move(ctrl), res.controllerSourcePath);
```
Ensure `#include <memory>` is available in GameThread.cpp (it is, transitively, but add it near the top if the build complains).

- [ ] **Step 4: Update AnimatorEditor.cpp's Get usage**

In `src/editor/src/panels/inspector/AnimatorEditor.cpp`, the live-readout site does `const AnimatorController* c = AnimatorControllerStore::Instance().Get(live->ControllerId);`. Change to:
```cpp
    std::shared_ptr<const AnimatorController> c = AnimatorControllerStore::Instance().Get(live->ControllerId);
```
The `if (c)` and all `c->states` / `c->params` usages work unchanged via `shared_ptr::operator->`.

- [ ] **Step 5: Build Engine + editor**
```
cmake --build --preset msvc-win64-vs2026-community --target Engine
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: both link clean. (The store API changed; the only callers are the evaluator, the drain, and AnimatorEditor — all updated. `test_worldserial`/`test_animator` don't touch the store, so they're unaffected.)

- [ ] **Step 6: Commit**
```
git -C C:/dev/clang-examples add src/engine/src/animation/AnimatorControllerStore.h src/engine/src/animation/AnimatorControllerStore.cpp src/engine/src/threading/GameThread.cpp src/engine/src/threading/GameThread.h src/editor/src/panels/inspector/AnimatorEditor.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): AnimatorControllerStore shared_ptr<const> + Reload + source-path tracking"
```

---

## Task 3: Pure helpers — to_json, RenameState, ValidateController + tests (TDD)

**Files:**
- Modify: `src/common/include/AnimatorController.h`
- Create: `src/editor/src/panels/AnimatorGraphLayout.h`
- Create: `tests/test_animgraph.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add `to_json`, `RenameState`, `ValidateController` to AnimatorController.h**

In `src/common/include/AnimatorController.h`, the `from_json(AnimatorController)` already exists. Add a matching `to_json` (after it) plus two pure helpers (near the other helpers, before the JSON section):

```cpp
// Rename a state and rewrite every transition referencing it (from/to). anyState ("*") is left alone.
inline void RenameState(AnimatorController& c, const std::string& oldName, const std::string& newName) {
    for (auto& s : c.states) if (s.name == oldName) s.name = newName;
    for (auto& t : c.transitions) {
        if (t.from == oldName) t.from = newName;
        if (t.to   == oldName) t.to   = newName;
    }
}

// Returns human-readable validation warnings (empty = clean). `clipResolves(stateClipIdx)` answers
// whether a state's clip handle is present in the AnimationStore (passed in so this stays engine-free).
inline std::vector<std::string> ValidateController(
        const AnimatorController& c,
        const std::function<bool(size_t /*stateIndex*/)>& clipResolves = {}) {
    std::vector<std::string> w;
    if (c.states.empty()) w.push_back("Controller has no states.");
    // Duplicate state names.
    for (size_t i = 0; i < c.states.size(); ++i)
        for (size_t j = i + 1; j < c.states.size(); ++j)
            if (c.states[i].name == c.states[j].name)
                w.push_back("Duplicate state name: " + c.states[i].name);
    // Transition endpoints + condition params.
    for (const auto& t : c.transitions) {
        if (t.from != "*" && FindState(c, t.from) < 0) w.push_back("Transition from unknown state: " + t.from);
        if (FindState(c, t.to) < 0)                    w.push_back("Transition to unknown state: " + t.to);
        for (const auto& cond : t.conditions) {
            bool declared = false;
            for (const auto& p : c.params) if (p.name == cond.paramName) { declared = true; break; }
            if (!declared) w.push_back("Condition uses undeclared param: " + cond.paramName);
        }
    }
    // Unresolved clips (optional, only if a resolver is supplied).
    if (clipResolves)
        for (size_t s = 0; s < c.states.size(); ++s)
            if (!c.states[s].clipKey.empty() && !clipResolves(s))
                w.push_back("State '" + c.states[s].name + "' clip does not resolve: " + c.states[s].clipKey);
    return w;
}
```
Add `to_json` right after the existing `from_json(AnimatorController)`:
```cpp
inline void to_json(nlohmann::json& j, const AnimatorController& c) {
    j = nlohmann::json{ {"name", c.name}, {"params", c.params}, {"states", c.states}, {"transitions", c.transitions} };
    // stateClipIds + any editorLayout are NOT written here (resolved at load / merged by the editor).
}
```
Ensure `#include <functional>` is present (it is — `SelectTransition` uses `std::function`).

> NOTE: SP5 may already define a `to_json(AnimatorController)`. CHECK FIRST: if one exists, do not duplicate — reuse it and ensure it emits name/params/states/transitions (and NOT stateClipIds). If the existing one differs, reconcile to the above.

- [ ] **Step 2: Create the layout helper `src/editor/src/panels/AnimatorGraphLayout.h`**

Pure (no ImGui), so it's testable. Reads/writes the `editorLayout` block of a controller's raw JSON:
```cpp
#pragma once
#include <string>
#include <unordered_map>
#include <array>
#include <nlohmann/json.hpp>

// Editor-only graph layout persisted in the .animctrl.json "editorLayout" block. Keyed by state name;
// the anyState node uses the reserved key "__any__". Runtime ignores this block.
struct AnimGraphLayout {
    std::unordered_map<std::string, std::array<float,2>> nodes; // name -> {x,y}
    std::array<float,2> pan{0.0f, 0.0f};
    float zoom = 1.0f;
};

inline AnimGraphLayout ReadLayout(const nlohmann::json& doc) {
    AnimGraphLayout L;
    if (!doc.contains("editorLayout")) return L;
    const auto& e = doc.at("editorLayout");
    if (e.contains("nodes"))
        for (auto it = e.at("nodes").begin(); it != e.at("nodes").end(); ++it)
            if (it.value().is_array() && it.value().size() == 2)
                L.nodes[it.key()] = { it.value()[0].get<float>(), it.value()[1].get<float>() };
    if (e.contains("pan") && e.at("pan").is_array() && e.at("pan").size() == 2)
        L.pan = { e.at("pan")[0].get<float>(), e.at("pan")[1].get<float>() };
    L.zoom = e.value("zoom", 1.0f);
    return L;
}

inline void WriteLayout(nlohmann::json& doc, const AnimGraphLayout& L) {
    nlohmann::json nodes = nlohmann::json::object();
    for (const auto& [name, xy] : L.nodes) nodes[name] = { xy[0], xy[1] };
    doc["editorLayout"] = { {"nodes", nodes}, {"pan", { L.pan[0], L.pan[1] }}, {"zoom", L.zoom} };
}
```

- [ ] **Step 3: Write the tests `tests/test_animgraph.cpp`**

```cpp
#include <cstdio>
#include "AnimatorController.h"
#include "AnimatorGraphLayout.h"

static int g_Failures = 0;
#define EXPECT(c) do{ if(!(c)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); ++g_Failures; } }while(0)

static AnimatorController Loco() {
    AnimatorController c; c.name = "Loco";
    c.params = { {"speed", AnimParamType::Float} };
    c.states = { {"Idle","Survey",false,true}, {"Walk","Walk",true,true}, {"Run","Run",true,true} };
    c.transitions = {
        {"Idle","Walk",0.2f,{{"speed",AnimCondOp::Greater,0.1f}}},
        {"Walk","Run", 0.2f,{{"speed",AnimCondOp::Greater,4.0f}}},
        {"*","Idle",   0.1f,{}},
    };
    return c;
}

static void T_tojson_roundtrip() {
    AnimatorController c = Loco();
    nlohmann::json j = c;                       // to_json
    AnimatorController r = j.get<AnimatorController>(); // from_json
    EXPECT(r.name == "Loco");
    EXPECT(r.params.size() == 1 && r.states.size() == 3 && r.transitions.size() == 3);
    EXPECT(r.states[1].cyclic && r.states[0].loop);
    EXPECT(r.transitions[2].from == "*" && r.transitions[2].to == "Idle");
    EXPECT(!j.contains("stateClipIds")); // resolved at load, never serialized
}

static void T_rename_rewrites_transitions() {
    AnimatorController c = Loco();
    RenameState(c, "Walk", "Stroll");
    EXPECT(FindState(c, "Stroll") == 1 && FindState(c, "Walk") < 0);
    EXPECT(c.transitions[0].to == "Stroll");   // Idle->Walk became Idle->Stroll
    EXPECT(c.transitions[1].from == "Stroll"); // Walk->Run became Stroll->Run
    EXPECT(c.transitions[2].from == "*");      // anyState untouched
}

static void T_validate() {
    EXPECT(ValidateController(Loco()).empty());                 // clean
    AnimatorController dup = Loco(); dup.states.push_back({"Idle","X",false,true});
    EXPECT(!ValidateController(dup).empty());                   // duplicate name
    AnimatorController dangling = Loco(); dangling.transitions.push_back({"Run","Ghost",0.2f,{}});
    EXPECT(!ValidateController(dangling).empty());              // to unknown state
    AnimatorController badparam = Loco();
    badparam.transitions[0].conditions[0].paramName = "nope";
    EXPECT(!ValidateController(badparam).empty());              // undeclared param
    AnimatorController empty; EXPECT(!ValidateController(empty).empty()); // no states
    // clip resolver: state 2 ("Run") reports unresolved
    auto resolver = [](size_t s){ return s != 2; };
    EXPECT(!ValidateController(Loco(), resolver).empty());
}

static void T_layout_roundtrip() {
    nlohmann::json doc; doc["name"] = "Loco";
    AnimGraphLayout L; L.nodes["Idle"] = {120,80}; L.nodes["__any__"] = {120,240}; L.pan = {5,6}; L.zoom = 1.5f;
    WriteLayout(doc, L);
    AnimGraphLayout R = ReadLayout(doc);
    EXPECT(R.nodes.at("Idle")[0] == 120 && R.nodes.at("Idle")[1] == 80);
    EXPECT(R.nodes.at("__any__")[1] == 240);
    EXPECT(R.pan[0] == 5 && R.zoom == 1.5f);
    // runtime from_json ignores editorLayout (no throw, graph fields default)
    AnimatorController c = doc.get<AnimatorController>();
    EXPECT(c.name == "Loco");
}

int main() {
    T_tojson_roundtrip();
    T_rename_rewrites_transitions();
    T_validate();
    T_layout_roundtrip();
    if (g_Failures) { std::fprintf(stderr, "test_animgraph: %d FAILURES\n", g_Failures); return 1; }
    std::printf("All anim-graph tests passed.\n");
    return 0;
}
```

- [ ] **Step 4: Register the test target**

In `tests/CMakeLists.txt`, after the `test_animator` block, add a `test_animgraph` block mirroring it, EXCEPT also add the editor panels dir to its includes (for `AnimatorGraphLayout.h`):
```cmake
add_executable(test_animgraph
    test_animgraph.cpp
)
target_link_libraries(test_animgraph PRIVATE
    CommonHeaders
    glm::glm
    ecs
)
target_include_directories(test_animgraph PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/editor/src/panels
)
target_compile_definitions(test_animgraph PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)
set_target_properties(test_animgraph PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```
(`AnimatorGraphLayout.h` includes only nlohmann + std, so it links fine without ImGui. Confirm `CommonHeaders`/`ecs` bring nlohmann transitively as the other test targets rely on.)

- [ ] **Step 5: Build + run**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_animgraph
./out/build/msvc-win64-vs2026-community/bin/Debug/test_animgraph.exe
```
Expected: `All anim-graph tests passed.`

- [ ] **Step 6: Commit**
```
git -C C:/dev/clang-examples add src/common/include/AnimatorController.h src/editor/src/panels/AnimatorGraphLayout.h tests/test_animgraph.cpp tests/CMakeLists.txt
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): to_json/RenameState/ValidateController + editorLayout helpers + tests"
```

---

## Task 4: AnimatorGraphPanel — window, picker, read-only render + live highlight

> Uses imgui-node-editor. The exact API calls (BeginNode/EndNode, BeginPin/EndPin, Link, SetNodePosition/GetNodePosition, NodeId/PinId/LinkId, Begin/End of the editor context) MUST be cross-checked against the vendored headers `third_party/imgui-node-editor/imgui_node_editor.h` and the API patterns there — read that header before writing the canvas code. This task renders the graph READ-ONLY (no editing yet) + the live highlight, to isolate the canvas-rendering integration from the editing logic.

**Files:**
- Create: `src/editor/src/panels/AnimatorGraphPanel.h`, `.cpp`
- Modify: `src/editor/src/app/ImGuiRenderer.h`, `.cpp`, `src/editor/CMakeLists.txt`

- [ ] **Step 1: Read the imgui-node-editor API header**

Read `third_party/imgui-node-editor/imgui_node_editor.h` fully. Note the namespace (`ax::NodeEditor`, commonly aliased `namespace ed = ax::NodeEditor;`), `ed::EditorContext* CreateEditor()/DestroyEditor()`, `ed::SetCurrentEditor()`, `ed::Begin()/End()`, `ed::BeginNode(id)/EndNode()`, `ed::BeginPin(id, ed::PinKind::Input/Output)/EndPin()`, `ed::Link(linkId, srcPinId, dstPinId)`, `ed::SetNodePosition(nodeId, ImVec2)`, `ed::GetNodePosition(nodeId)`. These are the calls Task 4-6 use; confirm their real signatures.

- [ ] **Step 2: Panel header `AnimatorGraphPanel.h`**

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "AnimatorController.h"
#include "AnimatorGraphLayout.h"

struct EditorContext;
namespace ax { namespace NodeEditor { struct EditorContext; } }

// Top-level "Animator Graph" window: visual node-graph editor for AnimatorController assets.
// Owns an imgui-node-editor context + a mutable working copy of the selected controller.
class AnimatorGraphPanel {
public:
    AnimatorGraphPanel();
    ~AnimatorGraphPanel();
    void Draw(const EditorContext& ctx, bool* open);
private:
    void LoadController(uint64_t handle);     // store -> working copy + layout (from source JSON)
    void SaveController();                    // working copy + layout -> source JSON + store Reload
    void ReloadFromDisk();                    // discard working copy, re-read source JSON

    ax::NodeEditor::EditorContext* m_Ed = nullptr;
    uint64_t            m_ControllerId = 0;   // store handle of the open controller (0 = none)
    std::string         m_ControllerKey;      // e.g. "models/Fox.gltf#animctrl"
    std::string         m_SourcePath;         // .animctrl.json path to save back to
    AnimatorController  m_Working;            // editable copy
    AnimGraphLayout     m_Layout;             // node positions for m_Working
    bool                m_Dirty = false;
    bool                m_LayoutApplied = false;  // apply m_Layout positions to the canvas once per (re)load
    std::vector<std::string> m_Warnings;      // last ValidateController result
    // Stable id mapping: node/pin/link ids for imgui-node-editor (derive deterministically from
    // state index + a kind tag so ids survive frame-to-frame; see .cpp).
};
```

- [ ] **Step 3: Panel impl `AnimatorGraphPanel.cpp` (read-only render + live highlight)**

Implement (consult the node-editor header for exact calls):
- ctor: `m_Ed = ed::CreateEditor(nullptr);` dtor: `ed::DestroyEditor(m_Ed);`
- `Draw(ctx, open)`:
  - `if (!*open) return;` then `ImGui::Begin("Animator Graph", open, ...)`.
  - **Controller picker** (top): a `BeginCombo` listing `AnimatorControllerStore::Instance().GetAssetList()`. Default-select the selected entity's controller: read `ctx.WorldSnapshot->GetComponent<AnimatorComponent>(selectedEntity)` (selected entity available via the same path AnimatorEditor uses — confirm how the panel gets the selected entity; `EditorContext` exposes it, or via `m_AppContext->SelectedEntity`). On selecting a controller handle != `m_ControllerId`, call `LoadController(handle)`.
  - **Toolbar**: Save (calls `SaveController()`, disabled if `!m_Dirty`), Reload-from-disk (`ReloadFromDisk()`), and the validation warning count/text (`m_Warnings`).
  - **Canvas**: `ed::SetCurrentEditor(m_Ed); ed::Begin("AnimatorGraphCanvas");`
    - For each state index `s`: `ed::BeginNode(NodeId(s));` draw the state name (ImGui::Text for now — editable in Task 5), an input pin `ed::BeginPin(InPinId(s), Input)`/`EndPin()` and output pin `OutPinId(s)`; `ed::EndNode();`. On first layout, if `m_Layout.nodes` has the state, `ed::SetNodePosition(NodeId(s), {x,y})` (apply once when (re)loaded — guard with a `m_LayoutApplied` flag so user drags aren't overridden each frame).
    - The anyState node: a node at a reserved id (e.g. `NodeId(kAnyStateNode)`), only an output pin.
    - For each transition `t` (index `ti`): `ed::Link(LinkId(ti), OutPinId(fromStateIndex or anyState), InPinId(toStateIndex));`.
    - **Live highlight**: if the selected entity uses `m_ControllerId`, read its `AnimatorComponent` (live, via WorldSnapshot): tint/flag the `CurrentState` node (e.g. push a distinct node border color) and, if `FromState>=0`, highlight the active transition link + draw its `w`. (imgui-node-editor exposes per-node/link styling via `ed::PushStyleColor`/link color args — use what the header provides; if styling is awkward read-only, at minimum draw an ImGui marker text on the active node.)
  - `ed::End(); ed::SetCurrentEditor(nullptr);` then `ImGui::End();`.
- Deterministic id scheme (file-local helpers): `NodeId(stateIndex) = stateIndex + 1`, `kAnyStateNode = 100000`, `InPinId(s) = 200000 + s`, `OutPinId(s) = 300000 + s`, `AnyOutPin = 300000 + kAnyStateNode`, `LinkId(ti) = 400000 + ti`. (Offsets keep the id spaces disjoint. Document them.)
- `LoadController(handle)`: `auto c = AnimatorControllerStore::Instance().Get(handle); if (!c) return; m_Working = *c; m_ControllerId = handle; m_ControllerKey = KeyForHandle(handle); m_SourcePath = SourcePathForHandle(handle);` then read layout from the source JSON file if it exists (`std::ifstream` → `nlohmann::json` → `ReadLayout`); set `m_LayoutApplied = false; m_Dirty = false;` and recompute `m_Warnings = ValidateController(m_Working)`.
- `SaveController()` / `ReloadFromDisk()`: leave as stubs that `SM_WARN("not yet implemented")` for THIS task (Task 6 implements them) — OR implement load-side now and save in Task 6. Keep Task 4 to render + load + highlight.

- [ ] **Step 4: Own + draw the panel in ImGuiRenderer**

In `src/editor/src/app/ImGuiRenderer.h`: `#include "AnimatorGraphPanel.h"` and add member `AnimatorGraphPanel m_AnimatorGraph;` near `m_MaterialManager`.
In `src/editor/src/app/ImGuiRenderer.cpp`: near the other panel draws (after `m_MaterialManager.Draw(ctx);`, ~line 512), add:
```cpp
        static bool s_ShowAnimatorGraphPanel = false;   // off by default (heavy panel)
        m_AnimatorGraph.Draw(ctx, &s_ShowAnimatorGraphPanel);
```
(Default OFF — unlike the always-on panels — so it only spins up the node-editor context when opened. If there's a Windows/View menu that toggles panels, wire a menu item to flip `s_ShowAnimatorGraphPanel`; if panels are toggled purely by the window's own close button + a persistent static, leaving it discoverable via a menu item is preferred — check MainMenuBar for where panel toggles live and add one if that's the pattern. If no such menu exists, set the default to `true` so it's reachable, and note it.)

- [ ] **Step 5: CMake + build**

Add `src/panels/AnimatorGraphPanel.cpp` to the editor sources in `src/editor/CMakeLists.txt` (next to `MaterialManagerPanel.cpp`). Then:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: editor builds + links clean (node-editor canvas renders; no editing yet).

- [ ] **Step 6: Commit**
```
git -C C:/dev/clang-examples add src/editor/src/panels/AnimatorGraphPanel.h src/editor/src/panels/AnimatorGraphPanel.cpp src/editor/src/app/ImGuiRenderer.h src/editor/src/app/ImGuiRenderer.cpp src/editor/CMakeLists.txt
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): AnimatorGraphPanel — window, picker, read-only canvas + live highlight"
```

---

## Task 5: AnimatorGraphPanel — editing operations

> Builds on Task 4's render. Adds mutation of `m_Working` (set `m_Dirty = true` + recompute `m_Warnings` on each change). Still no Save (Task 6). Consult the node-editor header for the interaction APIs: `ed::BeginCreate()/QueryNewLink(&a,&b)/AcceptNewItem()/EndCreate()`, `ed::BeginDelete()/QueryDeletedLink(&id)/QueryDeletedNode(&id)/AcceptDeletedItem()/EndDelete()`, `ed::GetSelectedNodes/Links`.

**Files:**
- Modify: `src/editor/src/panels/AnimatorGraphPanel.{h,cpp}`

- [ ] **Step 1: State node editing**

Inside each `BeginNode/EndNode`, replace the read-only `Text(name)` with editable widgets:
- name: `ImGui::InputText` into a per-node buffer; on commit, if changed call `RenameState(m_Working, old, new)` (so transitions follow) AND update `m_Layout.nodes` key (move the position entry from old→new name); set dirty.
- clip: `BeginCombo` listing the model's clips — `AnimationStore::Instance().GetAssetList()` filtered to keys starting with `<assetKey>#anim/` (derive `<assetKey>` by stripping the trailing `#animctrl` from `m_ControllerKey`); the displayed/stored value is the BARE name (strip the `<assetKey>#anim/` prefix). Set `m_Working.states[s].clipKey` to the bare name; dirty.
- `cyclic` + `loop` checkboxes → `m_Working.states[s].cyclic/loop`; dirty.
- A small "entry" badge if `s==0`; a context-menu / button "Set as entry" that moves state `s` to index 0 (rotate the vector; `stateClipIds` is recomputed at save, so only `states` order matters); dirty.

- [ ] **Step 2: Add / delete states + anyState**

- Toolbar "Add State": push a new `AnimState{ "State" + N, "", false, true }` to `m_Working.states`, place its node at a default canvas spot in `m_Layout`; dirty.
- Delete: handle `ed::QueryDeletedNode` in the `BeginDelete/EndDelete` block → on accept, remove that state from `m_Working.states` AND remove transitions referencing it (from/to == its name) AND its layout entry; dirty. (Never delete the anyState node via this path — ignore deletes of `kAnyStateNode`, or just drop its transitions.)
- "Add anyState" toolbar button (only if no anyState node shown) — anyState is implicit (it's just transitions with `from=="*"`); showing the node is a render concern. Make the anyState node always present when any `from=="*"` transition exists, and the button adds the first such capability (no-op on data until a link is drawn from it).

- [ ] **Step 3: Create / delete / reorder transitions**

- In `ed::BeginCreate()`: `QueryNewLink(&startPin, &endPin)` → map pin ids back to (stateIndex, kind); accept only Output→Input (a real from→to, or anyState-out→to). On `AcceptNewItem()`, append `AnimTransition{ fromName, toName, 0.2f, {} }` to `m_Working.transitions`; dirty.
- In `ed::BeginDelete()`: `QueryDeletedLink(&linkId)` → map to transition index → on accept, erase from `m_Working.transitions`; dirty.
- Selected-link inspector strip (right side or a popup when a link is selected via `ed::GetSelectedLinks`): `duration` drag; condition rows — for each `AnimCondition`: param `BeginCombo` (from `m_Working.params`), op `BeginCombo` (the 5 `AnimCondOp`), value `DragFloat`; "+"/"–" to add/remove a condition. Up/Down buttons to reorder the selected transition within `m_Working.transitions` (priority); dirty on any change.

- [ ] **Step 4: Params region**

A child region (e.g. left dock or a collapsing header): list `m_Working.params` (name `InputText` + type `Combo` Float/Bool/Trigger), "+"/"–" to add/remove a param. Renaming a param does NOT auto-rewrite conditions in v1 — but recompute `m_Warnings` so an orphaned condition surfaces as a validation warning; dirty.

- [ ] **Step 5: Build + commit**
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: clean.
```
git -C C:/dev/clang-examples add src/editor/src/panels/AnimatorGraphPanel.h src/editor/src/panels/AnimatorGraphPanel.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): AnimatorGraphPanel editing — states/clips/flags, transitions, conditions, params"
```

---

## Task 6: Save / Reload-from-disk + validation wiring

**Files:**
- Modify: `src/editor/src/panels/AnimatorGraphPanel.cpp`

- [ ] **Step 1: Implement `SaveController()`**

```cpp
void AnimatorGraphPanel::SaveController() {
    if (m_ControllerId == 0 || m_SourcePath.empty()) {
        SM_WARN("AnimatorGraphPanel: no controller / no source path — cannot save");
        return;
    }
    // 1. Capture current node positions from the canvas into m_Layout.
    ed::SetCurrentEditor(m_Ed);
    for (size_t s = 0; s < m_Working.states.size(); ++s) {
        const ImVec2 p = ed::GetNodePosition(NodeId(s));
        m_Layout.nodes[m_Working.states[s].name] = { p.x, p.y };
    }
    { const ImVec2 ap = ed::GetNodePosition(NodeId(kAnyStateNode)); m_Layout.nodes["__any__"] = { ap.x, ap.y }; }
    ed::SetCurrentEditor(nullptr);
    // 2. Serialize graph (to_json) + merge editorLayout into the same doc.
    nlohmann::json doc = m_Working;          // to_json(AnimatorController)
    WriteLayout(doc, m_Layout);
    // 3. Write the source .animctrl.json.
    std::ofstream f(m_SourcePath);
    if (!f) { SM_WARN("AnimatorGraphPanel: failed to open '%s' for write", m_SourcePath.c_str()); return; }
    f << doc.dump(2);
    f.close();
    // 4. Resolve clip names (mirror the GameThread drain) + live-reload the store.
    AnimatorController resolved = m_Working;
    const std::string assetKey = m_ControllerKey.substr(0, m_ControllerKey.size() - std::string("#animctrl").size());
    resolved.stateClipIds.assign(resolved.states.size(), 0);
    for (size_t s = 0; s < resolved.states.size(); ++s) {
        if (resolved.states[s].clipKey.empty()) continue;
        resolved.stateClipIds[s] = AssetKeyHash(assetKey + "#anim/" + resolved.states[s].clipKey);
    }
    AnimatorControllerStore::Instance().Reload(m_ControllerKey, std::move(resolved));
    m_Dirty = false;
    SM_TRACE("AnimatorGraphPanel: saved + reloaded '%s'", m_ControllerKey.c_str());
}
```
(Includes needed: `<fstream>`, `AssetKey.h`, `animation/AnimatorControllerStore.h`, the node-editor header for `ed::GetNodePosition`. `assetKey` strip assumes the key ends with `#animctrl` — it always does for store controllers.)

- [ ] **Step 2: Implement `ReloadFromDisk()`**

```cpp
void AnimatorGraphPanel::ReloadFromDisk() {
    if (m_SourcePath.empty()) { SM_WARN("AnimatorGraphPanel: no source path to reload"); return; }
    std::ifstream f(m_SourcePath);
    if (!f) { SM_WARN("AnimatorGraphPanel: failed to open '%s'", m_SourcePath.c_str()); return; }
    nlohmann::json doc; 
    try { f >> doc; } catch (const std::exception& ex) { SM_WARN("AnimatorGraphPanel: parse '%s': %s", m_SourcePath.c_str(), ex.what()); return; }
    m_Working = doc.get<AnimatorController>();
    m_Layout  = ReadLayout(doc);
    m_LayoutApplied = false;     // re-apply positions to the canvas next frame
    m_Dirty   = false;
    m_Warnings = ValidateController(m_Working);
}
```

- [ ] **Step 3: Wire validation + a clip resolver**

Where the panel recomputes `m_Warnings` (on load + after each edit), pass a clip resolver so unresolved-clip warnings show:
```cpp
const std::string assetKey = m_ControllerKey.substr(0, m_ControllerKey.size() - std::string("#animctrl").size());
m_Warnings = ValidateController(m_Working, [&](size_t s){
    if (m_Working.states[s].clipKey.empty()) return true;  // empty = intentional (no clip)
    return AnimationStore::Instance().Get(AssetKeyHash(assetKey + "#anim/" + m_Working.states[s].clipKey)) != nullptr;
});
```
Show `m_Warnings` in the toolbar (count + tooltip/list); if non-empty, a yellow "⚠ N" label. Save is still allowed (early-dev), but warnings are visible. Add `#include "animation/AnimationStore.h"`.

- [ ] **Step 4: Build + commit**
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: clean.
```
git -C C:/dev/clang-examples add src/editor/src/panels/AnimatorGraphPanel.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): AnimatorGraphPanel save (JSON write-back + live reload) + reload-from-disk + validation"
```

---

## Task 7: Full build, tests, manual smoke

**Files:** none (verification only).

- [ ] **Step 1: Full build**
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: all targets (incl. `imgui-node-editor`, `Engine`, `editor`, `runtime`, tests) link clean.

- [ ] **Step 2: Unit suites**
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_animgraph.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_animator.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
```
Expected: each prints its `All ... passed.` line. (`test_navagent` may be pre-existing RED — ignore it.)

- [ ] **Step 3: Manual smoke (report results, do not auto-pass)**

1. Launch `editor.exe`. Open the **Animator Graph** panel (menu or it defaults open).
2. Pick `models/Fox.gltf#animctrl` → graph renders: Idle/Walk/Run nodes + transition links, positioned from `editorLayout` (or auto-placed first time) + the anyState node if present.
3. Edit: rename a state, drag a new transition between two states, edit a condition value/op, toggle cyclic/loop, move nodes. The `⚠` warning count updates live.
4. **Save** → `assets/models/Fox.animctrl.json` on disk is updated (open it: graph changes + an `editorLayout` block present). A live Fox entity using the controller reflects the change next tick (no restart).
5. Select that Fox entity → its **current state node highlights** and the active transition lights up + shows weight as it moves (drive it / use the AnimatorEditor speed slider).
6. **Reload-from-disk** discards an unsaved edit (working copy reverts to the file).
7. Confirm the existing AnimatorEditor inspector + static/single-clip meshes are unaffected.

- [ ] **Step 4: Report** smoke results per step (pass/fail + any console warnings). If a step fails, debug via systematic-debugging; do not mark complete on failure. Commit any smoke fixes under the same author.

---

## Self-Review (completed during planning)

**Spec coverage:** vendor imgui-node-editor (T1) ✓; store shared_ptr+Reload+source-path (T2) ✓; to_json/RenameState/ValidateController + editorLayout (T3) ✓; panel window+picker+read-only render+live highlight (T4) ✓; full editing ops — states/clips/flags/entry/anyState, transitions/conditions/reorder, params (T5) ✓; Save (JSON write-back + editorLayout + live reload) + reload-from-disk + validation surfacing (T6) ✓; tests (T3) ✓; smoke (T7) ✓. Out-of-scope items (create-new controller, undo, blend-trees) correctly absent.

**Type consistency:** `AnimatorControllerStore::Get` → `shared_ptr<const AnimatorController>` used consistently (T2 evaluator + AnimatorEditor; T4/T6 panel `Get`/`Reload`/`SourcePathForHandle`). `RenameState`/`ValidateController`/`to_json` signatures defined T3, used T5/T6. `AnimGraphLayout`/`ReadLayout`/`WriteLayout` defined T3, used T4/T6. Id-scheme helpers (`NodeId`/`InPinId`/`OutPinId`/`LinkId`/`kAnyStateNode`) defined T4, used T5/T6. `m_LayoutApplied`/`m_Dirty`/`m_Warnings`/`m_SourcePath`/`m_ControllerKey` declared in the T4 header, used T4–T6.

**Verify-at-implementation points (flagged inline, not placeholders):** exact imgui-node-editor API signatures (read the vendored header — T4 Step 1, T5); the pinned library tag + exact file set (T1); whether a `to_json(AnimatorController)` already exists from SP5 (T3 Step 1 note); how the panel obtains the selected entity + whether a Windows/View menu toggles panels (T4 Step 4); confirm `m_LayoutApplied` guard added to the T4 header (declare it). Each has a concrete fallback in-task.

---

## Execution note

T1 (vendoring) is highest-risk (external dep + ImGui compat) — if it can't compile against the bundled ImGui, escalate before proceeding. T2 (engine seam) is small + deterministic. T3 is pure TDD. T4–T6 are the imgui-node-editor UI — read the vendored header first; exact canvas API calls are verified against it, not guessed. No `ECS.h` change anywhere → no editor-restart caveat (just normal relaunch after an editor rebuild).
