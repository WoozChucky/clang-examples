# SP5 — Data-Driven Animator (Animation State Machine) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A generic, data-driven animation state machine (Unity-Animator-mapped): arbitrary states/transitions authored as JSON, a generic engine evaluator, gameplay drives it only by setting parameters.

**Architecture:** Engine owns the reusable mechanism — `AnimatorController` JSON asset + store, `AnimatorComponent` builtin, and a GameThread evaluator that runs the graph (snapshot + cyclic-dual-cursor crossfade, phase-sync) → palette. Game owns the gameplay part — a `VelocityComponent`/`VelocitySystem` and a `PlayerAnimParamSystem` that set parameters. States/params are just strings/floats; the engine hardcodes no graph.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-community` preset ONLY), CMake, glm (depth `[0,1]`, RH, `GLM_ENABLE_EXPERIMENTAL`), nlohmann::json, NVRHI (unchanged — reuses the SP2 skinned palette transport).

**Spec:** `docs/superpowers/specs/2026-06-08-anim-statemachine-design.md`

---

## Conventions (apply to every task)

- **Build/test preset:** `msvc-win64-vs2026-community` ONLY (enterprise not installed). Binaries: `out/build/msvc-win64-vs2026-community/bin/Debug/`.
- **Commit author:** `Nuno Silva <nuno.levezinho@live.com.pt>` — pass `--author="Nuno Silva <nuno.levezinho@live.com.pt>"` on EVERY commit. NEVER the vinci-energies.net work email.
- **NEVER** `--no-verify`. **NEVER** `git add -A` / `git add .` — stage exact paths only. `assets/world.json`, `assets/engine_settings.json`, `assets/editor_preferences.json` are gitignored local files — never stage them.
- **git from git-bash:** cwd doesn't persist — use `git -C C:/dev/clang-examples ...`.
- Branch is already `feat/anim-statemachine`.
- **Logging:** `SM_TRACE`/`SM_WARN`/`SM_ERROR` (printf-style args work directly), never `printf`/`std::cout`. Log on degradation paths, never silent-skip.
- **ECS.h / new-component rule (from CLAUDE.md):** changing `ECS.h` (new component, struct layout) requires rebuilding `ecs.dll`, `editor`, AND `game`, then **restarting the editor**. Hot-reload does NOT cover `ECS.h` changes. Tasks 4–5 are the only ones that touch `ECS.h`; they are grouped so there is a single restart.
- A new builtin component must be registered in **four** places: `ECS_FOR_EACH_REGISTERED_COMPONENT` X-macro (`ECS.h`), `ComponentSerialization.h` (`to_json`/`from_json`), `src/ecs/src/ComponentSerializers.cpp` (`SerializerRegistry().Register<>`), and BOTH branches of `ECSCommands.h` (`ApplyComponentCommand` + `RemoveComponentByType`). Missing any is silent.
- New `.cpp`/`.h` files must be added to the owning `CMakeLists.txt` explicitly (no globbing): engine → `src/engine/CMakeLists.txt`; editor → `src/editor/CMakeLists.txt`; tests → `tests/CMakeLists.txt`.

---

## File Structure

**Create:**
- `src/common/include/AnimatorController.h` — graph types (`AnimParam`/`AnimState`/`AnimCondition`/`AnimTransition`/`AnimatorController`), enums, pure helpers (`EvalCondition`, `WrapPhase01`, `PhaseToTime`, `SelectTransition`), and nlohmann JSON (de)serialization for the controller. Header-only (mirrors `AnimationClip.h`).
- `src/engine/src/animation/AnimatorControllerStore.h` / `.cpp` — process-wide store of immutable controllers (mirror of `AnimationStore`).
- `src/game/src/VelocityComponent.h` — game-owned `VelocityComponent` + its to_json/from_json.
- `src/editor/src/panels/inspector/AnimatorEditor.h` / `.cpp` — inspector picker + live debug readout.
- `assets/models/Fox.animctrl.json` — the canonical demo/smoke controller.
- `tests/test_animator.cpp` — pure-fn + JSON-round-trip unit tests.

**Modify:**
- `src/common/include/ECS.h` — `#include "AnimationClip.h"`; add `AnimatorComponent`; add `X(AnimatorComponent)`; strip `ClipB`/`TimeB`/`BlendWeight` from `AnimationComponent`.
- `src/common/include/ComponentSerialization.h` — `AnimatorComponent` to_json/from_json; strip blend fields from `AnimationComponent` (de)serialize.
- `src/ecs/src/ComponentSerializers.cpp` — register `AnimatorComponent`.
- `src/common/include/ECSCommands.h` — `AnimatorComponent` branches in `ApplyComponentCommand` + `RemoveComponentByType`.
- `src/engine/src/threading/GameThread.cpp` — load `<model>.animctrl.json` in the model-load path; evaluator in `PublishPaletteFrame`.
- `src/engine/CMakeLists.txt` — add `AnimatorControllerStore.cpp`.
- `src/editor/src/panels/EcsInspectorPanel.cpp` — register `AnimatorEditor`.
- `src/editor/src/panels/inspector/AnimationEditor.cpp` — drop Clip B / Blend Weight / Time B UI.
- `src/editor/CMakeLists.txt` — add `AnimatorEditor.cpp`.
- `src/game/src/game.cpp` — `VelocitySystem`, `PlayerAnimParamSystem`, register both + `VelocityComponent` serializer/editor-hook.
- `tests/CMakeLists.txt` — add `test_animator`.

---

## Task 1: Animator graph types + pure helpers (common, TDD)

**Files:**
- Create: `src/common/include/AnimatorController.h`
- Create: `tests/test_animator.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the graph types + pure helpers header**

Create `src/common/include/AnimatorController.h`:

```cpp
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>

// Data-driven animation state machine (Unity-Animator-mapped). Pure data + pure helpers; no engine
// or ECS dependency, so the evaluator, JSON loader, and unit tests all share it. See
// docs/superpowers/specs/2026-06-08-anim-statemachine-design.md.

enum class AnimParamType { Float, Bool, Trigger };
// Bool/Trigger conditions ignore `op` and test "is set" (value != 0).
enum class AnimCondOp { Greater, Less, GreaterEqual, LessEqual, Equal };

struct AnimParam     { std::string name; AnimParamType type = AnimParamType::Float; };
struct AnimState     { std::string name; std::string clipKey; bool cyclic = false; }; // clipKey = BARE clip name
struct AnimCondition { std::string paramName; AnimCondOp op = AnimCondOp::Greater; float value = 0.0f; };
struct AnimTransition {
    std::string from;                       // "*" = anyState
    std::string to;
    float duration = 0.2f;                  // seconds
    std::vector<AnimCondition> conditions;  // ALL must hold (AND)
};
struct AnimatorController {
    std::string name;
    std::vector<AnimParam>      params;
    std::vector<AnimState>      states;       // index 0 = entry/default state
    std::vector<AnimTransition> transitions;
    std::vector<uint64_t>       stateClipIds; // resolved at load (parallel to states); 0 = unresolved
};

// --- pure helpers (unit-tested) ---

inline bool EvalCondition(AnimCondOp op, float paramValue, float threshold) {
    switch (op) {
        case AnimCondOp::Greater:      return paramValue >  threshold;
        case AnimCondOp::Less:         return paramValue <  threshold;
        case AnimCondOp::GreaterEqual: return paramValue >= threshold;
        case AnimCondOp::LessEqual:    return paramValue <= threshold;
        case AnimCondOp::Equal:        return paramValue == threshold;
    }
    return false;
}

inline float WrapPhase01(float phase) {
    if (phase >= 0.0f && phase < 1.0f) return phase;
    phase = std::fmod(phase, 1.0f);
    if (phase < 0.0f) phase += 1.0f;
    return phase;
}

inline float PhaseToTime(float phase, float duration) { return WrapPhase01(phase) * duration; }

// Find the index in controller.transitions of the first transition that should fire from
// `currentState`, given a parameter lookup. anyState ("*") transitions are evaluated first (in
// declared order), then the current state's outgoing (declared order). -1 = none.
// `stateName` maps a state index to its name; `param` returns a parameter's current value (0 if absent).
inline int SelectTransition(const AnimatorController& c, int currentState,
                            const std::function<float(const std::string&)>& param) {
    auto allHold = [&](const AnimTransition& t) {
        for (const auto& cond : t.conditions)
            if (!EvalCondition(cond.op, param(cond.paramName), cond.value)) return false;
        return true; // all conditions held (an empty condition list trivially fires)
    };
    const std::string current =
        (currentState >= 0 && currentState < (int)c.states.size()) ? c.states[currentState].name : std::string();
    // Pass 1: anyState.
    for (size_t i = 0; i < c.transitions.size(); ++i)
        if (c.transitions[i].from == "*" && c.transitions[i].to != current && allHold(c.transitions[i]))
            return (int)i;
    // Pass 2: outgoing from current.
    for (size_t i = 0; i < c.transitions.size(); ++i)
        if (c.transitions[i].from == current && allHold(c.transitions[i]))
            return (int)i;
    return -1;
}

// Index of a state by name; -1 if not found.
inline int FindState(const AnimatorController& c, const std::string& name) {
    for (size_t i = 0; i < c.states.size(); ++i) if (c.states[i].name == name) return (int)i;
    return -1;
}

// --- JSON (de)serialization ---

NLOHMANN_JSON_SERIALIZE_ENUM(AnimParamType, {
    {AnimParamType::Float, "Float"}, {AnimParamType::Bool, "Bool"}, {AnimParamType::Trigger, "Trigger"},
})
NLOHMANN_JSON_SERIALIZE_ENUM(AnimCondOp, {
    {AnimCondOp::Greater, "Greater"}, {AnimCondOp::Less, "Less"}, {AnimCondOp::GreaterEqual, "GreaterEqual"},
    {AnimCondOp::LessEqual, "LessEqual"}, {AnimCondOp::Equal, "Equal"},
})
inline void to_json(nlohmann::json& j, const AnimParam& p) { j = {{"name", p.name}, {"type", p.type}}; }
inline void from_json(const nlohmann::json& j, AnimParam& p) {
    j.at("name").get_to(p.name); p.type = j.value("type", AnimParamType::Float);
}
inline void to_json(nlohmann::json& j, const AnimState& s) { j = {{"name", s.name}, {"clipKey", s.clipKey}, {"cyclic", s.cyclic}}; }
inline void from_json(const nlohmann::json& j, AnimState& s) {
    j.at("name").get_to(s.name); s.clipKey = j.value("clipKey", std::string()); s.cyclic = j.value("cyclic", false);
}
inline void to_json(nlohmann::json& j, const AnimCondition& c) { j = {{"paramName", c.paramName}, {"op", c.op}, {"value", c.value}}; }
inline void from_json(const nlohmann::json& j, AnimCondition& c) {
    j.at("paramName").get_to(c.paramName); c.op = j.value("op", AnimCondOp::Greater); c.value = j.value("value", 0.0f);
}
inline void to_json(nlohmann::json& j, const AnimTransition& t) {
    j = {{"from", t.from}, {"to", t.to}, {"duration", t.duration}, {"conditions", t.conditions}};
}
inline void from_json(const nlohmann::json& j, AnimTransition& t) {
    j.at("from").get_to(t.from); j.at("to").get_to(t.to);
    t.duration = j.value("duration", 0.2f);
    t.conditions = j.value("conditions", std::vector<AnimCondition>{});
}
inline void to_json(nlohmann::json& j, const AnimatorController& c) {
    j = {{"name", c.name}, {"params", c.params}, {"states", c.states}, {"transitions", c.transitions}};
}
inline void from_json(const nlohmann::json& j, AnimatorController& c) {
    c.name = j.value("name", std::string());
    c.params      = j.value("params",      std::vector<AnimParam>{});
    c.states      = j.value("states",      std::vector<AnimState>{});
    c.transitions = j.value("transitions", std::vector<AnimTransition>{});
    // stateClipIds resolved at load time (engine), not from JSON.
}
```

Add `#include <cmath>` at the top as well (used by `WrapPhase01`/`PhaseToTime`).

- [ ] **Step 2: Write the failing test**

Create `tests/test_animator.cpp`:

```cpp
#include <cstdio>
#include "AnimatorController.h"

static int g_Failures = 0;
#define EXPECT(cond) do { if(!(cond)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); ++g_Failures; } } while(0)
static bool nearf(float a, float b) { return (a-b<1e-4f) && (b-a<1e-4f); }

static AnimatorController MakeLocomotion() {
    AnimatorController c;
    c.name = "Loco";
    c.params = { {"speed", AnimParamType::Float}, {"hit", AnimParamType::Trigger} };
    c.states = { {"Idle","Survey",false}, {"Walk","Walk",true}, {"Run","Run",true}, {"Hit","Hit",false} };
    c.transitions = {
        {"Idle","Walk",0.2f,{{"speed",AnimCondOp::Greater,0.1f}}},
        {"Walk","Run", 0.2f,{{"speed",AnimCondOp::Greater,4.0f}}},
        {"Run","Walk", 0.2f,{{"speed",AnimCondOp::LessEqual,4.0f}}},
        {"Walk","Idle",0.2f,{{"speed",AnimCondOp::LessEqual,0.1f}}},
        {"*","Hit",    0.1f,{{"hit",AnimCondOp::Greater,0.0f}}},   // anyState trigger
    };
    return c;
}

static void T_eval_condition() {
    EXPECT(EvalCondition(AnimCondOp::Greater, 5.0f, 4.0f));
    EXPECT(!EvalCondition(AnimCondOp::Greater, 4.0f, 4.0f));
    EXPECT(EvalCondition(AnimCondOp::LessEqual, 4.0f, 4.0f));
    EXPECT(EvalCondition(AnimCondOp::Less, 3.0f, 4.0f));
    EXPECT(EvalCondition(AnimCondOp::GreaterEqual, 4.0f, 4.0f));
    EXPECT(EvalCondition(AnimCondOp::Equal, 4.0f, 4.0f));
}

static void T_select_outgoing_first_match() {
    AnimatorController c = MakeLocomotion();
    int idle = FindState(c, "Idle");
    // speed below walk threshold -> no outgoing fires (anyState hit not set)
    auto p0 = [](const std::string&){ return 0.0f; };
    EXPECT(SelectTransition(c, idle, p0) == -1);
    // speed above walk threshold -> Idle->Walk (index 0)
    auto pSlow = [](const std::string& n){ return n=="speed"?1.0f:0.0f; };
    EXPECT(SelectTransition(c, idle, pSlow) == 0);
}

static void T_select_anystate_first() {
    AnimatorController c = MakeLocomotion();
    int walk = FindState(c, "Walk");
    // both Walk->Run (speed>4) AND anyState Hit (hit>0) satisfied -> anyState wins (evaluated first)
    auto p = [](const std::string& n){ return (n=="speed")?9.0f : (n=="hit")?1.0f : 0.0f; };
    int idx = SelectTransition(c, walk, p);
    EXPECT(idx >= 0 && c.transitions[idx].to == "Hit");
}

static void T_phase_math() {
    EXPECT(nearf(WrapPhase01(1.25f), 0.25f));
    EXPECT(nearf(WrapPhase01(-0.25f), 0.75f));
    EXPECT(nearf(PhaseToTime(0.5f, 2.0f), 1.0f));   // shared phase -> per-clip time
    EXPECT(nearf(PhaseToTime(0.5f, 0.8f), 0.4f));
}

static void T_json_roundtrip() {
    AnimatorController c = MakeLocomotion();
    nlohmann::json j = c;
    AnimatorController r = j.get<AnimatorController>();
    EXPECT(r.name == c.name);
    EXPECT(r.params.size() == c.params.size());
    EXPECT(r.states.size() == c.states.size() && r.states[1].cyclic == true);
    EXPECT(r.transitions.size() == c.transitions.size());
    EXPECT(r.transitions[4].from == "*" && r.transitions[4].to == "Hit");
    EXPECT(nearf(r.transitions[0].duration, 0.2f));
    EXPECT(r.transitions[0].conditions.size() == 1 && r.transitions[0].conditions[0].op == AnimCondOp::Greater);
}

int main() {
    T_eval_condition();
    T_select_outgoing_first_match();
    T_select_anystate_first();
    T_phase_math();
    T_json_roundtrip();
    if (g_Failures) { std::fprintf(stderr, "test_animator: %d FAILURES\n", g_Failures); return 1; }
    std::printf("All animator tests passed.\n");
    return 0;
}
```

- [ ] **Step 3: Register the test target**

In `tests/CMakeLists.txt`, after the `test_blend` block (ends ~line 869), add:

```cmake
add_executable(test_animator
    test_animator.cpp
)

target_link_libraries(test_animator PRIVATE
    CommonHeaders
    glm::glm
    ecs
)

target_include_directories(test_animator PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
)

target_compile_definitions(test_animator PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_animator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 4: Configure + build + run the test**

```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_animator
./out/build/msvc-win64-vs2026-community/bin/Debug/test_animator.exe
```
Expected: `All animator tests passed.`

- [ ] **Step 5: Commit**

```
git -C C:/dev/clang-examples add src/common/include/AnimatorController.h tests/test_animator.cpp tests/CMakeLists.txt
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): AnimatorController graph types + pure helpers + JSON (SP5)"
```

---

## Task 2: AnimatorControllerStore (engine)

**Files:**
- Create: `src/engine/src/animation/AnimatorControllerStore.h`, `src/engine/src/animation/AnimatorControllerStore.cpp`
- Modify: `src/engine/CMakeLists.txt`

- [ ] **Step 1: Write the store header**

Create `src/engine/src/animation/AnimatorControllerStore.h` (mirror `AnimationStore.h`):

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <mutex>
#include "Engine.h"          // ENGINE_API
#include "AnimatorController.h"

// Process-wide store of immutable AnimatorController assets, keyed by a stable hash handle
// (AssetKeyHash of "<modelKey>#animctrl"). GameThread-written (Add at load) + GameThread-read
// (evaluator). Entries immutable once added; mutex-guarded. Mirrors AnimationStore.
class ENGINE_API AnimatorControllerStore {
public:
    static AnimatorControllerStore& Instance();
    uint64_t Add(const std::string& key, AnimatorController controller); // de-dup by key
    const AnimatorController* Get(uint64_t handle) const;                // null if unknown
    std::string KeyForHandle(uint64_t handle) const;
    std::vector<std::pair<uint64_t, std::string>> GetAssetList() const;
private:
    AnimatorControllerStore() = default;
    struct Entry { std::string key; AnimatorController controller; };
    mutable std::mutex m_Mutex;
    std::unordered_map<uint64_t, Entry> m_ByHandle;
};
```

- [ ] **Step 2: Write the store impl**

Create `src/engine/src/animation/AnimatorControllerStore.cpp` (mirror `AnimationStore.cpp`):

```cpp
#include "animation/AnimatorControllerStore.h"
#include "AssetKey.h"

AnimatorControllerStore& AnimatorControllerStore::Instance() { static AnimatorControllerStore s; return s; }

uint64_t AnimatorControllerStore::Add(const std::string& key, AnimatorController controller) {
    const uint64_t handle = AssetKeyHash(key);
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    if (it != m_ByHandle.end()) return handle;
    m_ByHandle.emplace(handle, Entry{ key, std::move(controller) });
    return handle;
}
const AnimatorController* AnimatorControllerStore::Get(uint64_t handle) const {
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    return it == m_ByHandle.end() ? nullptr : &it->second.controller;
}
std::string AnimatorControllerStore::KeyForHandle(uint64_t handle) const {
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    return it == m_ByHandle.end() ? std::string() : it->second.key;
}
std::vector<std::pair<uint64_t, std::string>> AnimatorControllerStore::GetAssetList() const {
    std::scoped_lock lk(m_Mutex);
    std::vector<std::pair<uint64_t, std::string>> out;
    out.reserve(m_ByHandle.size());
    for (const auto& [h, e] : m_ByHandle) out.emplace_back(h, e.key);
    return out;
}
```

- [ ] **Step 3: Add the source to CMake**

In `src/engine/CMakeLists.txt`, find the line listing `src/animation/AnimationStore.cpp` (the engine target's explicit source list) and add directly below it:

```cmake
    src/animation/AnimatorControllerStore.cpp
```

(Search the file for `AnimationStore.cpp` to locate the exact spot. Match the existing relative-path style of the neighboring entries — they may be written as `src/animation/AnimationStore.cpp` or just `animation/AnimationStore.cpp`; mirror whichever is used.)

- [ ] **Step 4: Build the engine**

```
cmake --build --preset msvc-win64-vs2026-community --target Engine
```
Expected: links clean (no new symbols referenced yet — this just compiles the store in).

- [ ] **Step 5: Commit**

```
git -C C:/dev/clang-examples add src/engine/src/animation/AnimatorControllerStore.h src/engine/src/animation/AnimatorControllerStore.cpp src/engine/CMakeLists.txt
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): AnimatorControllerStore engine singleton (SP5)"
```

---

## Task 3: `AnimatorComponent` builtin + strip `AnimationComponent` blend fields (ECS)

> **This is the ECS.h task.** After it, rebuild `ecs` + `editor` + `game` and restart the editor. Combined with the strip so there is one restart. `AnimatorComponent` is non-trivially-copyable (holds `std::vector`s) — that is fine; `NameComponent` (holds `std::string`) is the existing precedent, and `ComponentData::Create` copy-constructs, so it flows through the command ring correctly.

**Files:**
- Modify: `src/common/include/ECS.h`
- Modify: `src/common/include/ComponentSerialization.h`
- Modify: `src/ecs/src/ComponentSerializers.cpp`
- Modify: `src/common/include/ECSCommands.h`

- [ ] **Step 1: Include BonePose + add `AnimatorComponent`, strip blend fields (`ECS.h`)**

In `src/common/include/ECS.h`, near the other animation includes / at the top include block, add:

```cpp
#include "AnimationClip.h"   // BonePose (for AnimatorComponent snapshot pose)
```

Replace the existing `AnimationComponent` (currently lines ~212–221) with the stripped single-clip form **plus** the new `AnimatorComponent`:

```cpp
struct AnimationComponent {
    uint64_t ClipId  = 0;     // AnimationStore handle (stable hash); 0 = none
    float    Time    = 0.0f;  // playback cursor, seconds
    float    Speed   = 1.0f;
    bool     Looping = true;
    bool     Playing = false;
};

// Data-driven animation state machine instance. Generic: the engine evaluator interprets the
// referenced AnimatorController (a data graph) using the per-instance parameter values the game
// writes each tick. Runtime-cursor fields are transient (recomputed; not serialized).
struct AnimatorComponent {
    uint64_t ControllerId = 0;                            // AnimatorControllerStore handle; 0 = none
    std::vector<std::pair<uint64_t, float>> Params;       // {FNV-1a hash of param name, value} (bool/trigger as 0/1)

    // --- runtime cursor (transient) ---
    int   CurrentState      = -1;                         // index into controller.states; -1 = uninitialized
    int   FromState         = -1;                         // -1 = not transitioning
    float TransitionElapsed = 0.0f;
    float TransitionDur     = 0.0f;
    bool  TransitionCyclic  = false;                      // both endpoints cyclic -> dual-cursor phase-sync
    float Phase             = 0.0f;                       // [0,1) locomotion phase (cyclic cursors)
    float StateTime         = 0.0f;                       // seconds into CurrentState clip (non-cyclic)
    std::vector<BonePose> SnapshotPose;                   // frozen "from" pose for snapshot blends
};
```

In the `ECS_FOR_EACH_REGISTERED_COMPONENT` X-macro (lines ~423–424), add after `X(AnimationComponent)`:

```cpp
    X(AnimationComponent) \
    X(AnimatorComponent)
```

(Ensure the previous line keeps its trailing `\` and the macro's last line has no trailing `\`.)

- [ ] **Step 2: Serialization — strip blend fields + add `AnimatorComponent` (`ComponentSerialization.h`)**

In `src/common/include/ComponentSerialization.h`, replace the `AnimationComponent` to_json/from_json (lines ~115–onwards) with the stripped form, and add `AnimatorComponent` right after:

```cpp
inline void to_json(nlohmann::json& j, const AnimationComponent& t) {
    j = nlohmann::json{ {"ClipId", t.ClipId}, {"Time", t.Time}, {"Speed", t.Speed},
                        {"Looping", t.Looping}, {"Playing", t.Playing} };
}
inline void from_json(const nlohmann::json& j, AnimationComponent& t) {
    t.ClipId  = j.value("ClipId",  (uint64_t)0);
    t.Time    = j.value("Time",    0.0f);
    t.Speed   = j.value("Speed",   1.0f);
    t.Looping = j.value("Looping", true);
    t.Playing = j.value("Playing", false);
    // ClipB/TimeB/BlendWeight intentionally dropped (SP4 demo scaffold); old saves' keys ignored.
}

inline void to_json(nlohmann::json& j, const AnimatorComponent& t) {
    nlohmann::json params = nlohmann::json::array();
    for (const auto& [h, v] : t.Params) params.push_back({ {"h", h}, {"v", v} });
    j = nlohmann::json{ {"ControllerId", t.ControllerId}, {"Params", params} };
}
inline void from_json(const nlohmann::json& j, AnimatorComponent& t) {
    t.ControllerId = j.value("ControllerId", (uint64_t)0);
    t.Params.clear();
    if (j.contains("Params"))
        for (const auto& e : j.at("Params"))
            t.Params.emplace_back(e.value("h", (uint64_t)0), e.value("v", 0.0f));
    // runtime cursor fields default-construct (transient).
}
```

(Use `j.value(...)` rather than `j.at(...)` for the `AnimationComponent` fields so old saves missing nothing still load, and new-schema robustness per the early-dev policy.)

- [ ] **Step 3: Register the serializer (`ComponentSerializers.cpp`)**

In `src/ecs/src/ComponentSerializers.cpp`, after `r.Register<AnimationComponent>("AnimationComponent", true);` (line ~27) add:

```cpp
        r.Register<AnimatorComponent>("AnimatorComponent", true);
```

- [ ] **Step 4: Command branches (`ECSCommands.h`)**

In `src/common/include/ECSCommands.h`, in `ApplyComponentCommand`, after the `AnimationComponent` branch (lines ~384–387) add:

```cpp
        } else if (componentData.Type == std::type_index(typeid(AnimatorComponent))) {
            if (auto* a = componentData.Get<AnimatorComponent>()) {
                world.AddComponent(entity, *a);
            }
```

In `RemoveComponentByType`, after the `AnimationComponent` branch (lines ~434–435) add:

```cpp
        } else if (typeIndex == std::type_index(typeid(AnimatorComponent))) {
            world.RemoveComponent<AnimatorComponent>(entity);
```

- [ ] **Step 5: Full rebuild + run ECS tests**

```
cmake --build --preset msvc-win64-vs2026-community --target ecs
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
cmake --build --preset msvc-win64-vs2026-community
```
Expected: `All ECS tests passed.`; full build (`Engine`, `ecs`, `editor`, `game`) links clean. (Editor restart happens at smoke time — Task 9.)

- [ ] **Step 6: Commit**

```
git -C C:/dev/clang-examples add src/common/include/ECS.h src/common/include/ComponentSerialization.h src/ecs/src/ComponentSerializers.cpp src/common/include/ECSCommands.h
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): AnimatorComponent builtin + strip AnimationComponent blend fields (SP5)"
```

---

## Task 4: Load `<model>.animctrl.json` (engine, GameThread)

> Controllers are sibling files of the model. Parse them in the worker thread (off the GameThread tick) into `ModelLoadResult`, then resolve clip names → `AnimationStore` handles and register into `AnimatorControllerStore` in the completed-job drain (where clips are already registered).

**Files:**
- Modify: `src/engine/src/threading/GameThread.cpp`
- Modify: `src/engine/src/threading/GameThread.h` (if `ModelLoadResult` is declared there) — otherwise edit wherever `ModelLoadResult` is defined.

- [ ] **Step 1: Add controller fields to `ModelLoadResult`**

Locate the `ModelLoadResult` struct (search `struct ModelLoadResult`). Add:

```cpp
    bool                hasController = false;
    AnimatorController  controller;        // clipKey = bare names; stateClipIds resolved at drain time
```

Ensure `#include "AnimatorController.h"` and `#include "animation/AnimatorControllerStore.h"` are present in `GameThread.cpp` (add near the existing `#include "animation/AnimationStore.h"`).

- [ ] **Step 2: Parse the sibling JSON in the worker**

In `WorkerThreadFunc`, after the clips are extracted into `result.clips` (search for `result.clips.push_back` / the SM_TRACE "Animation extracted" block, ~line 1053), add a block that loads the sibling controller file. `job.objPath` is the model path; the controller is `<objPath-without-extension>.animctrl.json`:

```cpp
        // Sibling animator controller (optional): "<model>.animctrl.json".
        {
            std::filesystem::path ctrlPath = std::filesystem::path(job.objPath).replace_extension(".animctrl.json");
            if (std::filesystem::exists(ctrlPath)) {
                try {
                    std::ifstream f(ctrlPath);
                    nlohmann::json j; f >> j;
                    result.controller = j.get<AnimatorController>();
                    result.hasController = true;
                    SM_TRACE("Animator controller loaded: '%s' (%zu states, %zu transitions)",
                             ctrlPath.string().c_str(), result.controller.states.size(),
                             result.controller.transitions.size());
                } catch (const std::exception& ex) {
                    SM_WARN("Failed to parse animator controller '%s': %s", ctrlPath.string().c_str(), ex.what());
                }
            }
        }
```

Ensure `<fstream>` and `<nlohmann/json.hpp>` are included in `GameThread.cpp` (search; add if missing — `<filesystem>` is already included).

- [ ] **Step 3: Resolve + register in the completed-job drain**

In the completed-job drain, immediately after the clips-registration loop (`for (auto& clip : res.clips) AnimationStore::Instance().Add(...)`, ~line 388–390), add:

```cpp
                    // Resolve the controller's bare clip names against this model's clips, then register it.
                    if (res.hasController) {
                        AnimatorController ctrl = res.controller;
                        ctrl.stateClipIds.resize(ctrl.states.size(), 0);
                        for (size_t s = 0; s < ctrl.states.size(); ++s) {
                            if (ctrl.states[s].clipKey.empty()) continue;
                            const std::string clipKey = res.assetKey + "#anim/" + ctrl.states[s].clipKey;
                            ctrl.stateClipIds[s] = AssetKeyHash(clipKey);
                            if (!AnimationStore::Instance().Get(ctrl.stateClipIds[s]))
                                SM_WARN("Animator '%s' state '%s': clip '%s' not found in AnimationStore",
                                        res.assetKey.c_str(), ctrl.states[s].name.c_str(), clipKey.c_str());
                        }
                        AnimatorControllerStore::Instance().Add(res.assetKey + "#animctrl", std::move(ctrl));
                    }
```

`AssetKeyHash` is declared in `AssetKey.h` — ensure it's included in `GameThread.cpp` (it is used elsewhere in this file already; verify).

- [ ] **Step 4: Build the engine**

```
cmake --build --preset msvc-win64-vs2026-community --target Engine
```
Expected: links clean.

- [ ] **Step 5: Commit**

```
git -C C:/dev/clang-examples add src/engine/src/threading/GameThread.cpp src/engine/src/threading/GameThread.h
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): load sibling <model>.animctrl.json into AnimatorControllerStore (SP5)"
```

---

## Task 5: Evaluator in `PublishPaletteFrame` (engine, GameThread)

> The generic graph runtime. Replaces the per-entity pose source: prefer `AnimatorComponent` → else single-clip `AnimationComponent` → else bind pose. Reuses the SP2 palette transport unchanged.

**Files:**
- Modify: `src/engine/src/threading/GameThread.cpp` (the `PublishPaletteFrame` function, lines ~709–752)

- [ ] **Step 1: Add an evaluator helper above `PublishPaletteFrame`**

Add this free function (file-static) directly above `void GameThread::PublishPaletteFrame(...)`. It mutates the cursor (caller passes the live component via `Modify`) and returns model-space globals. `#include "AnimatorController.h"` is already present from Task 4.

```cpp
// Advance + evaluate one AnimatorComponent against its controller for `dt`, returning model-space
// bone globals. Mutates the runtime cursor in `a`. `sk` is the entity's skeleton.
static std::vector<glm::mat4> EvaluateAnimator(const Skeleton& sk, const AnimatorController& c,
                                               AnimatorComponent& a, float dt) {
    auto paramLookup = [&](const std::string& name) -> float {
        const uint64_t h = AssetKeyHash(name);
        for (const auto& [ph, v] : a.Params) if (ph == h) return v;
        return 0.0f;
    };
    auto clipFor = [&](int state) -> const AnimationClip* {
        if (state < 0 || state >= (int)c.stateClipIds.size()) return nullptr;
        return AnimationStore::Instance().Get(c.stateClipIds[state]);
    };
    auto sampleState = [&](int state) -> std::vector<BonePose> {
        const AnimationClip* clip = clipFor(state);
        if (!clip) return SampleClipPoseFromBind(sk); // bind pose fallback (defined below)
        const bool cyclic = (state >= 0 && state < (int)c.states.size()) ? c.states[state].cyclic : false;
        const float t = cyclic ? PhaseToTime(a.Phase, clip->duration) : a.StateTime;
        return SampleClipPose(sk, *clip, t);
    };

    if (c.states.empty()) return ComputeBindPoseGlobals(sk);
    if (a.CurrentState < 0) { a.CurrentState = 0; a.Phase = 0.0f; a.StateTime = 0.0f; a.FromState = -1; }

    // Advance cursors.
    const AnimationClip* curClip = clipFor(a.CurrentState);
    const float cycleDur = (curClip && curClip->duration > 0.0f) ? curClip->duration : 1.0f;
    a.Phase = WrapPhase01(a.Phase + dt / cycleDur);
    a.StateTime += dt;
    if (curClip && !c.states[a.CurrentState].cyclic && a.StateTime > curClip->duration)
        a.StateTime = curClip->duration; // non-cyclic clamps (oneshots hold last frame)
    if (a.FromState >= 0) a.TransitionElapsed += dt;

    // Transition selection (only when not already transitioning).
    if (a.FromState < 0) {
        const int ti = SelectTransition(c, a.CurrentState, paramLookup);
        if (ti >= 0) {
            const AnimTransition& tr = c.transitions[ti];
            const int toState = FindState(c, tr.to);
            if (toState >= 0) {
                a.FromState         = a.CurrentState;
                a.TransitionElapsed = 0.0f;
                a.TransitionDur     = (tr.duration > 0.0f) ? tr.duration : 0.0001f;
                const bool fromCyclic = c.states[a.FromState].cyclic;
                const bool toCyclic   = c.states[toState].cyclic;
                a.TransitionCyclic    = fromCyclic && toCyclic;
                a.StateTime           = 0.0f;                       // reset new state's non-cyclic cursor
                if (!a.TransitionCyclic) a.SnapshotPose = sampleState(a.FromState); // freeze "from"
                a.CurrentState = toState;
                // Consume triggers referenced by this transition's conditions.
                for (const auto& cond : tr.conditions) {
                    const uint64_t h = AssetKeyHash(cond.paramName);
                    for (auto& pr : a.Params) if (pr.first == h) {
                        // only triggers reset; floats/bools persist. Trigger = declared AnimParamType::Trigger.
                        for (const auto& decl : c.params)
                            if (decl.type == AnimParamType::Trigger && AssetKeyHash(decl.name) == h) pr.second = 0.0f;
                    }
                }
            }
        }
    }

    // Produce the pose.
    std::vector<BonePose> pose;
    if (a.FromState < 0) {
        pose = sampleState(a.CurrentState);
    } else {
        const float w = std::min(a.TransitionElapsed / a.TransitionDur, 1.0f);
        if (a.TransitionCyclic) {
            const AnimationClip* fc = clipFor(a.FromState);
            const AnimationClip* tc = clipFor(a.CurrentState);
            const float tFrom = fc ? PhaseToTime(a.Phase, fc->duration) : 0.0f;
            const float tTo   = tc ? PhaseToTime(a.Phase, tc->duration) : 0.0f;
            std::vector<BonePose> from = fc ? SampleClipPose(sk, *fc, tFrom) : SampleClipPoseFromBind(sk);
            std::vector<BonePose> to   = tc ? SampleClipPose(sk, *tc, tTo)   : SampleClipPoseFromBind(sk);
            pose = BlendPoses(from, to, w);
        } else {
            pose = BlendPoses(a.SnapshotPose, sampleState(a.CurrentState), w);
        }
        if (w >= 1.0f) { a.FromState = -1; a.SnapshotPose.clear(); }
    }
    return PoseToGlobals(sk, pose);
}
```

- [ ] **Step 2: Add the bind-pose fallback helper**

`SampleClipPose` rests bones from `localBind`; for a missing clip we want the same rest pose without a clip. Add to `src/common/include/AnimationClip.h` (after `SampleClipPose`):

```cpp
// Rest pose (every bone = DecomposeTRS(localBind)) — used when a state has no resolvable clip.
inline std::vector<BonePose> SampleClipPoseFromBind(const Skeleton& sk) {
    std::vector<BonePose> pose(sk.bones.size());
    for (size_t b = 0; b < sk.bones.size(); ++b) pose[b] = DecomposeTRS(sk.bones[b].localBind);
    return pose;
}
```

- [ ] **Step 3: Wire the evaluator into `PublishPaletteFrame`**

In `PublishPaletteFrame` (lines ~709–752), replace the body of the `Each<SkeletonComponent>` lambda's pose-selection (the `if (anim && clipA) { ... } else { ... }` block, lines ~716–744) with the AnimatorComponent-first priority:

```cpp
        std::vector<glm::mat4> globals;
        const AnimatorComponent* animator = state.World.GetComponent<AnimatorComponent>(e);
        const AnimatorController* ctrl =
            (animator && animator->ControllerId) ? AnimatorControllerStore::Instance().Get(animator->ControllerId) : nullptr;
        if (animator && animator->ControllerId && !ctrl)
            SM_WARN("AnimatorComponent on entity %llu: controller %llu not in store",
                    (unsigned long long)e, (unsigned long long)animator->ControllerId);

        if (ctrl) {
            state.World.Modify<AnimatorComponent>(e, [&](AnimatorComponent& a) {
                globals = EvaluateAnimator(*sk, *ctrl, a, dt);
            });
        } else {
            const AnimationComponent* anim = state.World.GetComponent<AnimationComponent>(e);
            const AnimationClip* clipA = (anim && anim->ClipId) ? AnimationStore::Instance().Get(anim->ClipId) : nullptr;
            if (anim && clipA) {
                float tA = anim->Time;
                state.World.Modify<AnimationComponent>(e, [&](AnimationComponent& a) {
                    if (a.Playing && clipA->duration > 0.0f) {
                        a.Time += dt * a.Speed;
                        if (a.Looping) { a.Time = std::fmod(a.Time, clipA->duration); if (a.Time < 0.0f) a.Time += clipA->duration; }
                        else if (a.Time >= clipA->duration) { a.Time = clipA->duration; a.Playing = false; }
                    }
                    tA = a.Time;
                });
                globals = SampleAnimation(*sk, *clipA, tA);
            } else {
                if (anim && anim->ClipId && !clipA)
                    SM_WARN("AnimationComponent on entity %llu: clip %llu not in AnimationStore",
                            (unsigned long long)e, (unsigned long long)anim->ClipId);
                globals = ComputeBindPoseGlobals(*sk);
            }
        }
```

Keep the existing tail (palette computation + range append + `LatestPaletteFrame.store`) unchanged. Confirm `dt` is the function parameter name (it is: `PublishPaletteFrame(GameState& state, float dt)`).

- [ ] **Step 4: Build the engine**

```
cmake --build --preset msvc-win64-vs2026-community --target Engine
```
Expected: links clean.

- [ ] **Step 5: Commit**

```
git -C C:/dev/clang-examples add src/engine/src/threading/GameThread.cpp src/common/include/AnimationClip.h
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): generic animator evaluator in PublishPaletteFrame (SP5)"
```

---

## Task 6: `VelocityComponent` + `VelocitySystem` + `PlayerAnimParamSystem` (game)

> Game-owned. `.cpp`/`.h` changes only → hot-reloads (no editor restart). `VelocityComponent` is game-owned, so it registers via the boundary machinery (no `ecs.dll` change).

**Files:**
- Create: `src/game/src/VelocityComponent.h`
- Modify: `src/game/src/game.cpp`
- Modify: `src/game/CMakeLists.txt` (only if headers are listed; `.h` usually needs no entry — verify, add if the target lists headers)

- [ ] **Step 1: Write the component header**

Create `src/game/src/VelocityComponent.h`:

```cpp
#pragma once
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

// Game-owned post-collision velocity, finite-differenced from the actual transform each tick by
// VelocitySystem. The animator reads its horizontal magnitude as the "speed" parameter.
struct VelocityComponent {
    glm::vec3 Linear{0.0f};   // world units / second
    glm::vec3 PrevPos{0.0f};  // last tick's position
    bool      Init = false;   // false until PrevPos seeded
};

inline void to_json(nlohmann::json& j, const VelocityComponent& t) {
    j = nlohmann::json{ {"Linear", { t.Linear.x, t.Linear.y, t.Linear.z }} };
    // PrevPos/Init are transient; not persisted.
}
inline void from_json(const nlohmann::json& j, VelocityComponent& t) {
    if (j.contains("Linear") && j["Linear"].is_array() && j["Linear"].size() == 3) {
        t.Linear = glm::vec3(j["Linear"][0].get<float>(), j["Linear"][1].get<float>(), j["Linear"][2].get<float>());
    }
}
```

- [ ] **Step 2: Add `VelocitySystem` (game.cpp)**

In `src/game/src/game.cpp`, add `#include "VelocityComponent.h"` near the other component includes, and `#include "AnimatorController.h"` (for `AssetKeyHash`-free param hashing we'll reuse a local hash — see Step 3). Add this system class near `KinematicMovementSystem`:

```cpp
// Finite-differences actual post-resolution velocity into VelocityComponent. Physics phase, AFTER
// KinematicMovementSystem (which moves the transform). Robust to walls/nav: a stuck or stationary
// entity reads zero. Ungated — no-ops when no entity carries VelocityComponent.
class VelocitySystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const float dt = static_cast<float>(ctx.dt);
        if (dt <= 0.0f) return;
        ctx.world.Each<TransformComponent, VelocityComponent>([&](EntityId e) {
            const auto* tr = ctx.world.GetComponent<TransformComponent>(e);
            if (!tr) return;
            ctx.world.Modify<VelocityComponent>(e, [&](VelocityComponent& v) {
                if (!v.Init) { v.PrevPos = tr->Position; v.Init = true; v.Linear = glm::vec3(0.0f); return; }
                v.Linear  = (tr->Position - v.PrevPos) / dt;
                v.PrevPos = tr->Position;
            });
        });
    }
    const char* Name() const override { return "VelocitySystem"; }
    SystemPhase Phase() const override { return SystemPhase::Physics; }
};
```

- [ ] **Step 3: Add `PlayerAnimParamSystem` (game.cpp)**

The animator's param key is `AssetKeyHash(name)` (engine helper). Game.cpp can call `AssetKeyHash` — it's declared in `AssetKey.h` (already reachable; the game uses asset keys elsewhere). Add `#include "AssetKey.h"` if not present. Add:

```cpp
// Sets the player's animator "speed" parameter from horizontal velocity. PostSimulation, after
// Physics (so velocity is current). The ONLY gameplay->animation coupling; bosses/AI mirror this
// pattern over their own controllers + params.
class PlayerAnimParamSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const auto* gs = ctx.world.GetSingleton<GameStateComponent>();
        if (!gs || gs->Current != StateIndex(GameStateId::InLevel)) return;
        const uint64_t speedKey = AssetKeyHash("speed");
        ctx.world.Each<PlayerComponent, AnimatorComponent, VelocityComponent>([&](EntityId e) {
            const auto* v = ctx.world.GetComponent<VelocityComponent>(e);
            if (!v) return;
            const float speed = glm::length(glm::vec3(v->Linear.x, 0.0f, v->Linear.z));
            ctx.world.Modify<AnimatorComponent>(e, [&](AnimatorComponent& a) {
                for (auto& pr : a.Params) if (pr.first == speedKey) { pr.second = speed; return; }
                a.Params.emplace_back(speedKey, speed); // lazy-add if controller assignment didn't seed it
            });
        });
    }
    const char* Name() const override { return "PlayerAnimParamSystem"; }
    SystemPhase Phase() const override { return SystemPhase::PostSimulation; }
};
```

- [ ] **Step 4: Register the systems + the component**

In `GameRegisterSystems` (line ~910), add after `KinematicMovementSystem`:

```cpp
    s->Register(std::make_unique<VelocitySystem>());                 // Physics: finite-diff velocity (after Kinematic)
```

and after the camera systems (anywhere in the list, PostSimulation):

```cpp
    s->Register(std::make_unique<PlayerAnimParamSystem>());          // PostSimulation: velocity -> animator "speed"
```

In `GameRegisterComponents` (line ~930), add:

```cpp
    SerializerRegistry().Register<VelocityComponent>("VelocityComponent");
    SerializerRegistry().RegisterEditorHook("VelocityComponent", [](const EditorUI& ui, nlohmann::json& j) {
        return ui.DragFloat3(j, "Linear", 0.0f);  // read-only-ish display of last velocity
    });
```

If `EditorUI` has no `DragFloat3`, use whatever vec3 editor exists (search `struct EditorUI` for available methods; `DragFloat3`/`ColorEdit4` style). If none, drop the editor hook line (the serializer registration alone is enough; velocity is runtime data).

- [ ] **Step 5: Build the game library**

```
cmake --build --preset msvc-win64-vs2026-community --target game
```
Expected: builds `Game.dll`; editor hot-reloads it (if running) or picks it up on next launch.

- [ ] **Step 6: Commit**

```
git -C C:/dev/clang-examples add src/game/src/VelocityComponent.h src/game/src/game.cpp src/game/CMakeLists.txt
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): VelocityComponent + VelocitySystem + PlayerAnimParamSystem (SP5)"
```

---

## Task 7: `AnimatorEditor` inspector panel (editor)

**Files:**
- Create: `src/editor/src/panels/inspector/AnimatorEditor.h`, `.cpp`
- Modify: `src/editor/src/panels/EcsInspectorPanel.cpp`
- Modify: `src/editor/CMakeLists.txt`

- [ ] **Step 1: Write the editor header**

Mirror `src/editor/src/panels/inspector/AnimationEditor.h`. Open it first to copy the exact base-class/`m_St` pattern (`ComponentEditState<AnimatorComponent>` or similar). Create `AnimatorEditor.h` with the same shape, swapping the component type to `AnimatorComponent` and the class name to `AnimatorEditor`.

- [ ] **Step 2: Write the editor impl**

Create `src/editor/src/panels/inspector/AnimatorEditor.cpp`. Mirror `AnimationEditor.cpp`'s structure (AddDefault/Remove/DrawEditor with `m_St.Begin`, the ECS command ring on Apply). Body:

```cpp
#include "AnimatorEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include <string>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "animation/AnimatorControllerStore.h"
#include "AssetKey.h"
#include "lib.h"

void AnimatorEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, AnimatorComponent{});
    if (!ctx.App->ECSCommandRing.Push(addCmd))
        SM_WARN("ECS command queue full! Add component command dropped.");
}
void AnimatorEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<AnimatorComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd))
        SM_WARN("ECS command queue full! Remove component command dropped.");
}
void AnimatorEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    if (!m_St.Begin(ctx, e)) return;

    // Controller picker (list built only while the combo is open).
    std::string current = AnimatorControllerStore::Instance().KeyForHandle(m_St.edit.ControllerId);
    if (current.empty()) current = "(none)";
    if (ImGui::BeginCombo("Controller", current.c_str())) {
        const auto list = AnimatorControllerStore::Instance().GetAssetList();
        if (list.empty()) ImGui::TextDisabled("No controllers loaded");
        for (const auto& [handle, key] : list) {
            const bool sel = (handle == m_St.edit.ControllerId);
            if (ImGui::Selectable(key.c_str(), sel)) {
                m_St.edit.ControllerId = handle;
                // Seed params from the controller's declarations so the debug view shows them.
                m_St.edit.Params.clear();
                if (const auto* c = AnimatorControllerStore::Instance().Get(handle))
                    for (const auto& p : c->params) m_St.edit.Params.emplace_back(AssetKeyHash(p.name), 0.0f);
                m_St.modified = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Live debug readout (read from the live component, not the edit copy).
    if (const auto* live = ctx.World ? ctx.World->GetComponent<AnimatorComponent>(e) : nullptr) {
        if (const auto* c = AnimatorControllerStore::Instance().Get(live->ControllerId)) {
            auto stateName = [&](int s){ return (s>=0 && s<(int)c->states.size()) ? c->states[s].name.c_str() : "-"; };
            ImGui::SeparatorText("Runtime");
            ImGui::Text("State: %s", stateName(live->CurrentState));
            if (live->FromState >= 0) {
                const float w = live->TransitionDur > 0 ? live->TransitionElapsed / live->TransitionDur : 1.0f;
                ImGui::Text("Transition: %s -> %s  w=%.2f%s", stateName(live->FromState),
                            stateName(live->CurrentState), w, live->TransitionCyclic ? " (phase-sync)" : "");
            }
            ImGui::Text("Phase: %.3f", live->Phase);
            ImGui::SeparatorText("Params (drag to test)");
            for (size_t i = 0; i < c->params.size(); ++i) {
                const auto& decl = c->params[i];
                const uint64_t h = AssetKeyHash(decl.name);
                float val = 0.0f;
                for (const auto& pr : live->Params) if (pr.first == h) { val = pr.second; break; }
                // Editable copy in m_St for manual testing (applied via Apply button).
                float* edit = nullptr;
                for (auto& pr : m_St.edit.Params) if (pr.first == h) edit = &pr.second;
                if (!edit) { m_St.edit.Params.emplace_back(h, val); edit = &m_St.edit.Params.back().second; }
                if (decl.type == AnimParamType::Float) {
                    if (ImGui::DragFloat(decl.name.c_str(), edit, 0.05f, 0.0f, 20.0f)) m_St.modified = true;
                } else {
                    bool b = (*edit != 0.0f);
                    if (ImGui::Checkbox(decl.name.c_str(), &b)) { *edit = b ? 1.0f : 0.0f; m_St.modified = true; }
                }
                ImGui::SameLine(); ImGui::TextDisabled("(live %.2f)", val);
            }
        }
    }

    ImGui::Spacing();
    if (m_St.modified) ImGui::TextColored(ImVec4(1,1,0,1), "* Modified (not yet saved)");
    if (ImGui::Button("Apply Changes##Animator", ImVec2(150, 0))) {
        ECSCommand modifyCmd = ECSCommand::ModifyComponent(e, m_St.edit);
        if (!ctx.App->ECSCommandRing.Push(modifyCmd))
            SM_WARN("ECS command queue full! Modify command dropped.");
        m_St.modified = false;
    }
}
```

> **Caveat to verify when implementing:** check `EditorContext` for how to read the live component (`ctx.World`/`ctx.world` — match the actual field; `AnimationEditor.cpp` doesn't read live state, so confirm the snapshot accessor used elsewhere in the panel code, e.g. `EcsInspectorPanel`). If no live-snapshot accessor is readily available, fall back to showing the debug readout from `m_St.edit` only (still shows assigned controller + lets you drag params), and note it. The picker + Apply path is the must-have; the live readout is best-effort.

- [ ] **Step 3: Register the editor**

In `src/editor/src/panels/EcsInspectorPanel.cpp`: add `#include "inspector/AnimatorEditor.h"` next to the other inspector includes (~line 18), and `m_Editors.push_back(std::make_unique<AnimatorEditor>());` next to the others (~line 38).

- [ ] **Step 4: Add to CMake**

In `src/editor/CMakeLists.txt`, find `src/panels/inspector/AnimationEditor.cpp` in the editor target's source list and add below it:

```cmake
    src/panels/inspector/AnimatorEditor.cpp
```

(Mirror the exact relative-path style of the neighbor.)

- [ ] **Step 5: Build the editor**

```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: builds `editor.exe`.

- [ ] **Step 6: Commit**

```
git -C C:/dev/clang-examples add src/editor/src/panels/inspector/AnimatorEditor.h src/editor/src/panels/inspector/AnimatorEditor.cpp src/editor/src/panels/EcsInspectorPanel.cpp src/editor/CMakeLists.txt
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): AnimatorEditor inspector panel (picker + live debug) (SP5)"
```

---

## Task 8: Fox demo controller asset + `AnimationEditor` cleanup

**Files:**
- Create: `assets/models/Fox.animctrl.json`
- Modify: `src/editor/src/panels/inspector/AnimationEditor.cpp` (already partly covered — ensure blend UI is gone)

- [ ] **Step 1: Confirm Fox clip names**

The controller references clips by bare name. Confirm Fox's clip names (the asset has Survey/Walk/Run per project memory). At runtime they register as `models/Fox.gltf#anim/<name>`; the bare names are `Survey`, `Walk`, `Run`. If a launch log shows different names (watch for the `SM_TRACE("Animation extracted: '...#anim/<name>'")` lines), use those exact names in the JSON.

- [ ] **Step 2: Write the controller asset**

Create `assets/models/Fox.animctrl.json`:

```json
{
  "name": "Fox",
  "params": [ { "name": "speed", "type": "Float" } ],
  "states": [
    { "name": "Idle", "clipKey": "Survey", "cyclic": false },
    { "name": "Walk", "clipKey": "Walk",   "cyclic": true  },
    { "name": "Run",  "clipKey": "Run",    "cyclic": true  }
  ],
  "transitions": [
    { "from": "Idle", "to": "Walk", "duration": 0.2, "conditions": [ { "paramName": "speed", "op": "Greater",   "value": 0.1 } ] },
    { "from": "Walk", "to": "Run",  "duration": 0.2, "conditions": [ { "paramName": "speed", "op": "Greater",   "value": 4.0 } ] },
    { "from": "Run",  "to": "Walk", "duration": 0.2, "conditions": [ { "paramName": "speed", "op": "LessEqual", "value": 4.0 } ] },
    { "from": "Walk", "to": "Idle", "duration": 0.2, "conditions": [ { "paramName": "speed", "op": "LessEqual", "value": 0.1 } ] }
  ]
}
```

- [ ] **Step 3: Verify `AnimationEditor.cpp` blend UI removed**

Task 3 stripped the fields; `AnimationEditor.cpp` still references `m_St.edit.ClipB`/`BlendWeight`/`TimeB` (lines ~43–58) which no longer exist → it won't compile. Remove the "Clip B (blend target)" combo, the "Blend Weight" slider, and the "Time B" drag (lines ~43–58 in the current file), leaving the single-clip controls (Clip, Playing, Looping, Speed, Time) + the Apply block.

- [ ] **Step 4: Build the editor**

```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: compiles clean (confirms the blend-field removal is complete).

- [ ] **Step 5: Commit**

```
git -C C:/dev/clang-examples add assets/models/Fox.animctrl.json src/editor/src/panels/inspector/AnimationEditor.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): Fox demo controller asset + AnimationEditor single-clip cleanup (SP5)"
```

---

## Task 9: Full build, test suite, manual smoke

**Files:** none (verification only).

- [ ] **Step 1: Full clean-ish build (all targets)**

```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: `Engine`, `ecs`, `game`, `editor`, `runtime`, and all test targets link clean.

- [ ] **Step 2: Run the unit suites**

```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_animator.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_skeleton.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_animation.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_blend.exe
```
Expected: each prints its `All ... tests passed.` line. (Per project memory, `test_navagent` may be pre-existing RED — ignore that one.)

- [ ] **Step 3: Manual smoke (editor) — RESTART the editor (ECS.h changed in Task 3)**

1. Launch `out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe`.
2. Watch the console for `Animator controller loaded: '...Fox.animctrl.json' (3 states, 4 transitions)` and three `Animation extracted: 'models/Fox.gltf#anim/Survey|Walk|Run'` lines. If clip names differ, fix `Fox.animctrl.json` (Task 8 Step 1) and reload.
3. Select an entity; assign the rigged **Fox** mesh + a `SkeletonComponent` (Fox skeleton) as in SP4. Scale ~0.05 (Fox authored in cm). Add an `AnimatorComponent`; in the **AnimatorEditor**, pick the `models/Fox.gltf#animctrl` controller → Apply.
4. In the AnimatorEditor, drag the **speed** param: 0 → state `Idle`; >0.1 → crossfades to `Walk`; >4.0 → crossfades to `Run`; back down reverses. The Runtime readout shows the active transition + `w` and `(phase-sync)` on Walk↔Run.
5. Add `PlayerComponent` + `VelocityComponent` to that entity (so it's the player). Enter Play mode, drive with WASD: locomotion state follows real speed; **walk↔run is phase-synced (no foot-slide)** — the SP4 regression is fixed.
6. Confirm static meshes + any single-clip `AnimationComponent` preview entities still render correctly (no regression).

- [ ] **Step 4: Report smoke results**

Report what was observed at each smoke step (pass/fail + any console warnings). Do NOT mark the task complete on a failed smoke — debug via systematic-debugging.

- [ ] **Step 5: (No commit)** — verification task. If smoke surfaced fixes, commit them with descriptive messages under the same author.

---

## Self-Review (completed during planning)

**Spec coverage:** Architecture (Tasks 2–5) ✓; data model `AnimatorController`/`AnimatorComponent`/`VelocityComponent` (Tasks 1, 3, 6) ✓; evaluator incl. anyState/triggers, snapshot + cyclic-dual-cursor + phase-sync, promote (Task 5) ✓; priority Animator→AnimationComponent→bind (Task 5 Step 3) ✓; error/degradation logs (Tasks 4–5) ✓; JSON authoring + clip-name resolution (Tasks 1, 4, 8) ✓; editor picker + live debug (Task 7) ✓; `AnimationComponent` strip + AnimationEditor cleanup (Tasks 3, 8) ✓; game velocity + param-setter (Task 6) ✓; tests `test_animator` + JSON round-trip (Task 1) ✓; Fox.animctrl.json deliverable (Task 8) ✓; smoke (Task 9) ✓.

**Type consistency:** `AnimatorComponent` fields used identically in ECS.h (Task 3), evaluator (Task 5), game param-setter (Task 6), editor (Task 7). `AnimatorController`/`stateClipIds` defined in Task 1, populated in Task 4, read in Task 5. `SampleClipPoseFromBind` defined in Task 5 Step 2, used in Task 5 Step 1. Param key = `AssetKeyHash(name)` consistently (Tasks 5, 6, 7). `EvaluateAnimator` signature matches its call site.

**Known verify-at-implementation points (flagged inline, not placeholders):** exact CMake source-list path style (Tasks 2/7); `EditorUI` vec3 method name (Task 6 Step 4); live-snapshot accessor in the inspector (Task 7 Step 2); Fox clip names (Task 8 Step 1). Each has a concrete fallback in-task.

---

## Execution note

Order matters for rebuild cost: Task 3 is the only `ECS.h` change (forces `ecs`+`editor`+`game` rebuild + editor restart) — everything after it is `.cpp`/asset work. Tasks 1–2 are pure additions (no restart). Do them in order.
