# Combat/Ability Prototype Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the animation state machine its first non-locomotion consumer — LMB triggers a one-shot `Attack` state that auto-returns via exit-time, with movement locked during a game-owned root window — proving the gameplay↔animation seam with zero ability data in the engine.

**Architecture:** Engine adds two generic primitives: **exit-time** transitions (a non-cyclic state's outgoing transition fires when its clip reaches a normalized point) and a shared **normalized-time** helper. The game owns everything ability: a `PlayerAbilitySystem` sets an `attack` Trigger on LMB; a root-policy helper reads the animator cursor (state name + normalized time) and locks movement. No ability/AoE/root data enters the engine.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-community` preset ONLY), CMake, nlohmann::json, ImGui + imgui-node-editor.

**Spec:** `docs/superpowers/specs/2026-06-08-ability-prototype-design.md`

---

## Conventions (apply to every task)

- **Build/test preset:** `msvc-win64-vs2026-community` ONLY. Binaries: `out/build/msvc-win64-vs2026-community/bin/Debug/`.
- **Commit author:** EVERY commit `--author="Nuno Silva <nuno.levezinho@live.com.pt>"`. NEVER the vinci-energies.net work email.
- **NEVER** `--no-verify`. **NEVER** `git add -A`/`git add .` — stage exact paths. Never stage `assets/world.json`, `assets/engine_settings.json`, `assets/editor_preferences.json` (gitignored local files). `assets/models/Fox.animctrl.json` IS a tracked deliverable (verify with `git -C C:/dev/clang-examples check-ignore` — it is NOT ignored).
- **git from git-bash:** use `git -C C:/dev/clang-examples ...`.
- Branch is already `feat/ability-prototype`.
- **Logging:** `SM_TRACE`/`SM_WARN`/`SM_ERROR`, never `printf`/`cout`. Log degradation, never silent-skip.
- **No `ECS.h` change** in this feature → no component-restart caveat. `AnimatorComponent` already carries `CurrentState`/`StateTime`/`Phase` (read by the game directly).
- New source files added to the owning `CMakeLists.txt` explicitly.

---

## File Structure

**Modify:**
- `src/common/include/AnimatorController.h` — `AnimTransition` += `hasExitTime`/`exitTime`; `SelectTransition` gains a defaulted `normalizedTime` arg + exit-time logic; `AnimTransition` JSON; `ValidateController` anyState-exit-time warning; new `NormalizedStateTime` helper.
- `src/engine/src/threading/GameThread.cpp` — evaluator computes current-state normalized time + passes to `SelectTransition`.
- `src/editor/src/panels/AnimatorGraphPanel.cpp` — exit-time control on the selected-link inspector (non-anyState links).
- `src/editor/src/panels/inspector/AnimatorEditor.cpp` — show normalized time in the live readout.
- `src/game/src/game.cpp` — `PlayerAbilitySystem`; `ShouldRootMovement` helper + root-policy/movement-lock; `PlayerMovementSystem` lock guard; register the new system.
- `assets/models/Fox.animctrl.json` — `attack` param + `Attack` state + transitions.
- `tests/test_animator.cpp` — exit-time + `NormalizedStateTime` tests.
- `tests/test_animgraph.cpp` — `hasExitTime`/`exitTime` JSON round-trip.

**Create:**
- `src/game/src/AbilityRoot.h` — the pure `ShouldRootMovement` helper (+ `kAttackRootEnd`), so it's unit-testable without a World and a real ability config has an obvious home.
- `tests/test_abilityroot.cpp` — tests for `ShouldRootMovement`.

---

## Task 1: Exit-time data + SelectTransition + JSON + validation + NormalizedStateTime (common, TDD)

**Files:**
- Modify: `src/common/include/AnimatorController.h`
- Modify: `tests/test_animator.cpp`, `tests/test_animgraph.cpp`

- [ ] **Step 1: Add the failing tests first**

In `tests/test_animator.cpp`, add these test functions and call them from `main` (mirror the existing `EXPECT`/`nearf` style already in the file):

```cpp
static void T_exit_time() {
    AnimatorController c;
    c.params = { {"attack", AnimParamType::Trigger} };
    c.states = { {"Idle","Survey",false,true}, {"Attack","Survey",false,false} };
    // Attack -> Idle fires on exit-time only (no conditions).
    c.transitions = {
        {"*","Attack",0.1f,{{"attack",AnimCondOp::Greater,0.0f}}},   // anyState trigger (index 0)
        {"Attack","Idle",0.15f,{}},                                  // index 1 — will get exit-time
    };
    c.transitions[1].hasExitTime = true;
    c.transitions[1].exitTime    = 1.0f;
    const int attack = FindState(c, "Attack");
    auto noParams = [](const std::string&){ return 0.0f; };
    // Below exitTime -> Attack->Idle does NOT fire.
    EXPECT(SelectTransition(c, attack, noParams, 0.5f) == -1);
    // At/after exitTime -> fires (index 1).
    EXPECT(SelectTransition(c, attack, noParams, 1.0f) == 1);
    EXPECT(SelectTransition(c, attack, noParams, 1.5f) == 1);
    // anyState transition IGNORES exit-time: set hasExitTime on the anyState transition and confirm
    // it still fires purely on its condition regardless of normalizedTime.
    AnimatorController c2 = c;
    c2.transitions[0].hasExitTime = true; c2.transitions[0].exitTime = 1.0f;
    auto attackSet = [](const std::string& n){ return n=="attack"?1.0f:0.0f; };
    EXPECT(SelectTransition(c2, FindState(c2,"Idle"), attackSet, 0.0f) == 0); // anyState fires at norm=0
    // Exit-time combined with a condition: only when BOTH hold.
    AnimatorController c3;
    c3.params = { {"go", AnimParamType::Float} };
    c3.states = { {"A","x",false,false}, {"B","x",false,true} };
    c3.transitions = { {"A","B",0.2f,{{"go",AnimCondOp::Greater,0.5f}}} };
    c3.transitions[0].hasExitTime = true; c3.transitions[0].exitTime = 0.8f;
    auto go = [](const std::string& n){ return n=="go"?1.0f:0.0f; };
    EXPECT(SelectTransition(c3, 0, go, 0.5f) == -1);  // cond holds but before exitTime
    EXPECT(SelectTransition(c3, 0, [](const std::string&){return 0.0f;}, 0.9f) == -1); // past exitTime but cond fails
    EXPECT(SelectTransition(c3, 0, go, 0.9f) == 0);   // both
}

static void T_normalized_state_time() {
    AnimState noncyc{"A","x",false,true};
    AnimState cyc{"B","x",true,true};
    EXPECT(nearf(NormalizedStateTime(noncyc, 0.5f, 0.0f, 2.0f), 0.25f));
    EXPECT(nearf(NormalizedStateTime(noncyc, 5.0f, 0.0f, 2.0f), 1.0f));   // clamped
    EXPECT(nearf(NormalizedStateTime(cyc,    0.0f, 0.3f, 2.0f), 0.3f));   // cyclic uses phase
    EXPECT(nearf(NormalizedStateTime(noncyc, 1.0f, 0.0f, 0.0f), 0.0f));   // 0-duration guard
}
```
Add `T_exit_time();` and `T_normalized_state_time();` to `main`.

In `tests/test_animgraph.cpp`, extend `T_tojson_roundtrip` (or add a focused test) to cover exit-time round-trip — after building a controller, set `c.transitions[0].hasExitTime = true; c.transitions[0].exitTime = 0.75f;`, round-trip via json, and assert:
```cpp
EXPECT(r.transitions[0].hasExitTime == true && nearf(r.transitions[0].exitTime, 0.75f));
// a transition authored WITHOUT the keys defaults to hasExitTime=false, exitTime=1.0
nlohmann::json jt = { {"from","A"}, {"to","B"}, {"duration",0.2} };
AnimTransition dt = jt.get<AnimTransition>();
EXPECT(dt.hasExitTime == false && nearf(dt.exitTime, 1.0f));
```

- [ ] **Step 2: Run the tests — verify they FAIL to compile (new fields/args don't exist yet)**

```
cmake --build --preset msvc-win64-vs2026-community --target test_animator
```
Expected: compile error (`hasExitTime`/`NormalizedStateTime` undefined, `SelectTransition` arity). That's the failing state.

- [ ] **Step 3: Add the `AnimTransition` fields**

In `src/common/include/AnimatorController.h`, change `struct AnimTransition` to:
```cpp
struct AnimTransition {
    std::string from;                       // "*" = anyState
    std::string to;
    float duration = 0.2f;                  // seconds
    std::vector<AnimCondition> conditions;  // ALL must hold (AND)
    bool  hasExitTime = false;              // when true (non-anyState only), also require the FROM state's
    float exitTime    = 1.0f;               // normalized progress [0,1] to have reached exitTime
};
```

- [ ] **Step 4: Add the `NormalizedStateTime` helper**

After `PhaseToTime` (around line 62), add:
```cpp
// Normalized progress [0,1] of a state's clip. Non-cyclic: stateTime/duration (clamped). Cyclic: the
// wrapped phase. 0 when the clip has no duration. Shared by the evaluator, the game's root policy,
// and the editor readout so they agree on "how far through".
inline float NormalizedStateTime(const AnimState& s, float stateTime, float phase, float clipDuration) {
    if (clipDuration <= 0.0f) return 0.0f;
    return s.cyclic ? WrapPhase01(phase) : std::min(stateTime / clipDuration, 1.0f);
}
```
(`<algorithm>` for `std::min` — add the include at the top of the header if not present.)

- [ ] **Step 5: Extend `SelectTransition` with the normalized-time arg + exit-time gate**

Replace `SelectTransition` (lines ~67-85) with:
```cpp
// Find the index of the first transition that should fire from `currentState`. anyState ("*") first
// (declared order), then the current state's outgoing (declared order). A transition fires when all
// its conditions hold AND (for non-anyState transitions with hasExitTime) the current state's
// `normalizedTime` has reached exitTime. anyState transitions ignore exitTime (no single FROM state
// to measure). -1 = none.
inline int SelectTransition(const AnimatorController& c, int currentState,
                            const std::function<float(const std::string&)>& param,
                            float normalizedTime = 0.0f) {
    auto allHold = [&](const AnimTransition& t) {
        for (const auto& cond : t.conditions)
            if (!EvalCondition(cond.op, param(cond.paramName), cond.value)) return false;
        return true;
    };
    auto fires = [&](const AnimTransition& t, bool isAnyState) {
        if (!allHold(t)) return false;
        if (t.hasExitTime && !isAnyState && normalizedTime < t.exitTime) return false;
        return true;
    };
    const std::string current =
        (currentState >= 0 && currentState < (int)c.states.size()) ? c.states[currentState].name : std::string();
    // Pass 1: anyState.
    for (size_t i = 0; i < c.transitions.size(); ++i)
        if (c.transitions[i].from == "*" && c.transitions[i].to != current && fires(c.transitions[i], true))
            return (int)i;
    // Pass 2: outgoing from current.
    for (size_t i = 0; i < c.transitions.size(); ++i)
        if (c.transitions[i].from == current && fires(c.transitions[i], false))
            return (int)i;
    return -1;
}
```
(The defaulted `normalizedTime=0.0f` keeps existing call sites — including other tests — compiling unchanged; with `hasExitTime` defaulting false, their behavior is identical.)

- [ ] **Step 6: Exit-time in the AnimTransition JSON**

Replace the `AnimTransition` to_json/from_json (lines ~152-159):
```cpp
inline void to_json(nlohmann::json& j, const AnimTransition& t) {
    j = {{"from", t.from}, {"to", t.to}, {"duration", t.duration}, {"conditions", t.conditions},
         {"hasExitTime", t.hasExitTime}, {"exitTime", t.exitTime}};
}
inline void from_json(const nlohmann::json& j, AnimTransition& t) {
    j.at("from").get_to(t.from); j.at("to").get_to(t.to);
    t.duration     = j.value("duration", 0.2f);
    t.conditions   = j.value("conditions", std::vector<AnimCondition>{});
    t.hasExitTime  = j.value("hasExitTime", false);
    t.exitTime     = j.value("exitTime", 1.0f);
}
```

- [ ] **Step 7: Validate anyState + exit-time misuse**

In `ValidateController`, inside the `for (const auto& t : c.transitions)` loop (after the from/to checks, ~line 116), add:
```cpp
        if (t.from == "*" && t.hasExitTime)
            w.push_back("anyState transition to '" + t.to + "' sets hasExitTime, which is ignored (no single FROM state).");
```

- [ ] **Step 8: Run the tests — verify PASS**

```
cmake --build --preset msvc-win64-vs2026-community --target test_animator
./out/build/msvc-win64-vs2026-community/bin/Debug/test_animator.exe
cmake --build --preset msvc-win64-vs2026-community --target test_animgraph
./out/build/msvc-win64-vs2026-community/bin/Debug/test_animgraph.exe
```
Expected: `All animator tests passed.` and `All anim-graph tests passed.`

- [ ] **Step 9: Commit**
```
git -C C:/dev/clang-examples add src/common/include/AnimatorController.h tests/test_animator.cpp tests/test_animgraph.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): exit-time transitions + NormalizedStateTime helper (+ tests, validation)"
```

---

## Task 2: Evaluator passes normalized time to SelectTransition (engine)

**Files:**
- Modify: `src/engine/src/threading/GameThread.cpp` (`EvaluateAnimator`)

- [ ] **Step 1: Compute current-state normalized time + pass it**

In `EvaluateAnimator`, find the transition-selection block (around line 791-792):
```cpp
    if (a.FromState < 0) {
        const int ti = SelectTransition(c, a.CurrentState, paramLookup);
```
Replace those two lines with (reuses `curClip` from the advance block above + `NormalizedStateTime`):
```cpp
    if (a.FromState < 0) {
        const float curNorm = (a.CurrentState >= 0 && a.CurrentState < (int)c.states.size() && curClip)
            ? NormalizedStateTime(c.states[a.CurrentState], a.StateTime, a.Phase, curClip->duration)
            : 0.0f;
        const int ti = SelectTransition(c, a.CurrentState, paramLookup, curNorm);
```
`curClip` is already in scope (declared at the advance block, line ~774). `NormalizedStateTime` comes from `AnimatorController.h` (already included). No other change in the block.

- [ ] **Step 2: Build the engine**
```
cmake --build --preset msvc-win64-vs2026-community --target Engine
```
Expected: links clean.

- [ ] **Step 3: Commit**
```
git -C C:/dev/clang-examples add src/engine/src/threading/GameThread.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): evaluator feeds current-state normalized time to SelectTransition (exit-time)"
```

---

## Task 3: Editor — exit-time control + normalized-time readout

**Files:**
- Modify: `src/editor/src/panels/AnimatorGraphPanel.cpp`
- Modify: `src/editor/src/panels/inspector/AnimatorEditor.cpp`

- [ ] **Step 1: Exit-time control on the selected-link inspector**

In `AnimatorGraphPanel.cpp`, in the selected-link block, after the `duration` DragFloat (line ~603) and before `ImGui::SeparatorText("Conditions");` (line ~605), add:
```cpp
                // Exit-time (ignored for anyState transitions — no single FROM state).
                if (t.from == "*") {
                    ImGui::TextDisabled("(exit-time N/A for anyState)");
                } else {
                    if (ImGui::Checkbox("has exit time", &t.hasExitTime)) MarkEdited();
                    if (t.hasExitTime) {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(120.0f);
                        if (ImGui::SliderFloat("exit time", &t.exitTime, 0.0f, 1.0f)) MarkEdited();
                    }
                }
```

- [ ] **Step 2: Normalized time in the AnimatorEditor live readout**

In `src/editor/src/panels/inspector/AnimatorEditor.cpp`, the runtime readout shows `Phase: %.3f`. Add a normalized-time line. After the `ImGui::Text("Phase: %.3f", live->Phase);` line, add (resolve the current state's clip duration via `AnimationStore`):
```cpp
            if (live->CurrentState >= 0 && live->CurrentState < (int)c->states.size()) {
                float dur = 0.0f;
                if (live->CurrentState < (int)c->stateClipIds.size())
                    if (const auto* clip = AnimationStore::Instance().Get(c->stateClipIds[live->CurrentState]))
                        dur = clip->duration;
                ImGui::Text("Norm: %.3f", NormalizedStateTime(c->states[live->CurrentState], live->StateTime, live->Phase, dur));
            }
```
Ensure `#include "animation/AnimationStore.h"` and `AnimatorController.h` (for `NormalizedStateTime`) are included in `AnimatorEditor.cpp` (AnimationStore likely already is; add if the build complains). `c` here is the `shared_ptr<const AnimatorController>` already resolved in that readout block.

- [ ] **Step 3: Build the editor**
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: links clean.

- [ ] **Step 4: Commit**
```
git -C C:/dev/clang-examples add src/editor/src/panels/AnimatorGraphPanel.cpp src/editor/src/panels/inspector/AnimatorEditor.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): graph-editor exit-time control + normalized-time readout"
```

---

## Task 4: Game — ability input, root policy, movement lock (TDD for the pure helper)

**Files:**
- Create: `src/game/src/AbilityRoot.h`, `tests/test_abilityroot.cpp`
- Modify: `src/game/src/game.cpp`, `tests/CMakeLists.txt`

- [ ] **Step 1: Create the pure root-policy helper `src/game/src/AbilityRoot.h`**

```cpp
#pragma once
#include <string>

// Game-owned ability root policy. The engine never sees this — it only exposes the animator cursor
// (state name + normalized time); the game decides what roots movement. PROTOTYPE: one hardcoded rule
// (root the first kAttackRootEnd of the "Attack" state — a fireball-cast-style partial window). A real
// game would carry per-ability windows on an AbilityComponent (whole-state for heavy/channel, partial
// for cast); they all plug in HERE by varying this function, with NO engine change.
inline constexpr float kAttackRootEnd = 0.6f;

inline bool ShouldRootMovement(const std::string& stateName, float normalizedTime) {
    return stateName == "Attack" && normalizedTime < kAttackRootEnd;
}
```

- [ ] **Step 2: Write the failing test `tests/test_abilityroot.cpp`**

```cpp
#include <cstdio>
#include "AbilityRoot.h"

static int g_Failures = 0;
#define EXPECT(c) do{ if(!(c)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); ++g_Failures; } }while(0)

int main() {
    EXPECT(ShouldRootMovement("Attack", 0.0f));
    EXPECT(ShouldRootMovement("Attack", 0.59f));
    EXPECT(!ShouldRootMovement("Attack", 0.6f));   // window end is exclusive
    EXPECT(!ShouldRootMovement("Attack", 0.9f));
    EXPECT(!ShouldRootMovement("Idle", 0.1f));     // other states never root
    EXPECT(!ShouldRootMovement("Walk", 0.1f));
    if (g_Failures) { std::fprintf(stderr, "test_abilityroot: %d FAILURES\n", g_Failures); return 1; }
    std::printf("All ability-root tests passed.\n");
    return 0;
}
```

- [ ] **Step 3: Register the test (`tests/CMakeLists.txt`)**

After the `test_animgraph` block, add a `test_abilityroot` block mirroring it, with the game src dir on the include path:
```cmake
add_executable(test_abilityroot
    test_abilityroot.cpp
)
target_link_libraries(test_abilityroot PRIVATE
    CommonHeaders
    glm::glm
    ecs
)
target_include_directories(test_abilityroot PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/game/src
)
target_compile_definitions(test_abilityroot PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)
set_target_properties(test_abilityroot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 4: Build + run the test (verify pass)**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_abilityroot
./out/build/msvc-win64-vs2026-community/bin/Debug/test_abilityroot.exe
```
Expected: `All ability-root tests passed.`

- [ ] **Step 5: Add `PlayerAbilitySystem` + root/movement-lock to game.cpp**

READ `game.cpp` first: the existing `PlayerAnimParamSystem` (the `speed` param-write + `AssetKeyHash`), `PlayerMovementSystem` (WASD → `MoveIntentComponent`), `GameRegisterSystems`, the `InputStateComponent` mouse fields (`MousePressed[MOUSE_BUTTON_LEFT]`, used elsewhere in the file), and the `InLevel` gate idiom (`gs->Current != StateIndex(GameStateId::InLevel)`).

Add `#include "AbilityRoot.h"`, `#include "AnimatorController.h"`, `#include "animation/AnimationStore.h"`, `#include "animation/AnimatorControllerStore.h"` near the other includes (some may already be present).

Add this system class near `PlayerAnimParamSystem`:
```cpp
// LMB -> set the player's "attack" Trigger param (edge-detected). PostSimulation (after velocity/speed
// are set), gated to InLevel. The ONLY ability input wiring for the prototype.
class PlayerAbilitySystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const auto* gs = ctx.world.GetSingleton<GameStateComponent>();
        if (!gs || gs->Current != StateIndex(GameStateId::InLevel)) { m_PrevLmb = false; return; }
        const auto* in = ctx.world.GetSingleton<InputStateComponent>();
        const bool lmb = in && in->MousePressed[MOUSE_BUTTON_LEFT];
        const bool edge = lmb && !m_PrevLmb;
        m_PrevLmb = lmb;
        if (!edge) return;
        const uint64_t attackKey = AssetKeyHash("attack");
        ctx.world.Each<PlayerComponent, AnimatorComponent>([&](EntityId e) {
            ctx.world.Modify<AnimatorComponent>(e, [&](AnimatorComponent& a) {
                for (auto& pr : a.Params) if (pr.first == attackKey) { pr.second = 1.0f; return; }
                a.Params.emplace_back(attackKey, 1.0f);
            });
        });
    }
    const char* Name() const override { return "PlayerAbilitySystem"; }
    SystemPhase Phase() const override { return SystemPhase::PostSimulation; }
private:
    bool m_PrevLmb = false;
};
```

Add a root-policy system that reads the animator cursor and sets a movement-lock flag. Use a tiny game-owned component for the lock (so the mover reads it cleanly). At the top of game.cpp near other game components, add:
```cpp
struct MovementLockedComponent { bool Locked = false; };
```
(Game-owned, transient — no serializer needed; it's set/read within a tick. If the codebase requires every component to be registered for ECS storage, it already supports header-instantiable game components, so `AddComponent`/`Modify`/`GetComponent` work without ecs.dll changes — mirror how `VelocityComponent` is used.)

Then the system:
```cpp
// Reads the player's animator cursor (current state name + normalized time) and applies the game-owned
// root policy (AbilityRoot.h). PostSimulation, AFTER PlayerAbilitySystem/PublishPaletteFrame ordering
// is irrelevant here because it reads the PREVIOUS tick's published cursor on the component. Sets
// MovementLockedComponent; PlayerMovementSystem honors it. Engine has zero ability knowledge.
class AbilityRootSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const auto* gs = ctx.world.GetSingleton<GameStateComponent>();
        if (!gs || gs->Current != StateIndex(GameStateId::InLevel)) return;
        ctx.world.Each<PlayerComponent, AnimatorComponent>([&](EntityId e) {
            const auto* a = ctx.world.GetComponent<AnimatorComponent>(e);
            if (!a) return;
            bool rooted = false;
            if (const auto ctrl = AnimatorControllerStore::Instance().Get(a->ControllerId)) {
                const int s = a->CurrentState;
                if (s >= 0 && s < (int)ctrl->states.size()) {
                    float dur = 0.0f;
                    if (s < (int)ctrl->stateClipIds.size())
                        if (const auto* clip = AnimationStore::Instance().Get(ctrl->stateClipIds[s]))
                            dur = clip->duration;
                    const float norm = NormalizedStateTime(ctrl->states[s], a->StateTime, a->Phase, dur);
                    rooted = ShouldRootMovement(ctrl->states[s].name, norm);
                }
            }
            if (!ctx.world.HasComponent<MovementLockedComponent>(e))
                ctx.world.AddComponent(e, MovementLockedComponent{});
            ctx.world.Modify<MovementLockedComponent>(e, [&](MovementLockedComponent& m){ m.Locked = rooted; });
        });
    }
    const char* Name() const override { return "AbilityRootSystem"; }
    SystemPhase Phase() const override { return SystemPhase::PostSimulation; }
};
```
NOTE on `AnimatorControllerStore::Get` returning `shared_ptr<const AnimatorController>` — bind with `auto`/`if (const auto ctrl = ...)` and use `ctrl->...` (the recent store change). Confirm the game can include `animation/AnimatorControllerStore.h` (it's an engine header exposed via ENGINE_API; the game already calls engine singletons like `AnimationStore` — mirror that include path).

- [ ] **Step 6: Guard `PlayerMovementSystem` with the lock**

In `PlayerMovementSystem::Update`, inside the `ctx.world.Each<PlayerComponent, TransformComponent>([&](EntityId e){ ... })` lambda, at the very top (before computing `desiredDelta`), add:
```cpp
            if (const auto* lock = ctx.world.GetComponent<MovementLockedComponent>(e); lock && lock->Locked)
                return; // rooted by an ability this tick — emit no move intent
```

- [ ] **Step 7: Register the new systems**

In `GameRegisterSystems`, after the `PlayerAnimParamSystem` registration, add:
```cpp
    s->Register(std::make_unique<PlayerAbilitySystem>());            // PostSimulation: LMB -> attack trigger
    s->Register(std::make_unique<AbilityRootSystem>());             // PostSimulation: cursor -> movement lock
```
(`AbilityRootSystem` runs after `PlayerAbilitySystem`; both PostSimulation; registration order = run order within a phase. `PlayerMovementSystem` is Simulation, so it reads the lock set on the PREVIOUS tick's PostSimulation — a 1-tick latency that's invisible at 60Hz. If you prefer zero latency, that's fine as-is for a prototype; do not reorder phases.)

- [ ] **Step 8: Build the game**
```
cmake --build --preset msvc-win64-vs2026-community --target game
```
Expected: builds `Game.dll`.

- [ ] **Step 9: Commit**
```
git -C C:/dev/clang-examples add src/game/src/AbilityRoot.h src/game/src/game.cpp tests/test_abilityroot.cpp tests/CMakeLists.txt
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): PlayerAbilitySystem (LMB->attack trigger) + cursor-read root policy + movement lock"
```

---

## Task 5: Fox controller `Attack` state

**Files:**
- Modify: `assets/models/Fox.animctrl.json`

- [ ] **Step 1: Add the attack param, Attack state, and transitions**

Edit `assets/models/Fox.animctrl.json`. Add `{ "name": "attack", "type": "Trigger" }` to `params`. Add the `Attack` state to `states`. Add the two transitions to `transitions`. Final file:
```json
{
  "name": "Fox",
  "params": [
    { "name": "speed", "type": "Float" },
    { "name": "attack", "type": "Trigger" }
  ],
  "states": [
    { "name": "Idle",   "clipKey": "Survey", "cyclic": false, "loop": true },
    { "name": "Walk",   "clipKey": "Walk",   "cyclic": true,  "loop": true },
    { "name": "Run",    "clipKey": "Run",    "cyclic": true,  "loop": true },
    { "name": "Attack", "clipKey": "Survey", "cyclic": false, "loop": false }
  ],
  "transitions": [
    { "from": "Idle", "to": "Walk", "duration": 0.2, "conditions": [ { "paramName": "speed", "op": "Greater",   "value": 0.1 } ] },
    { "from": "Walk", "to": "Run",  "duration": 0.2, "conditions": [ { "paramName": "speed", "op": "Greater",   "value": 4.0 } ] },
    { "from": "Run",  "to": "Walk", "duration": 0.2, "conditions": [ { "paramName": "speed", "op": "LessEqual", "value": 4.0 } ] },
    { "from": "Walk", "to": "Idle", "duration": 0.2, "conditions": [ { "paramName": "speed", "op": "LessEqual", "value": 0.1 } ] },
    { "from": "*",      "to": "Attack", "duration": 0.1,  "conditions": [ { "paramName": "attack", "op": "Greater", "value": 0.0 } ] },
    { "from": "Attack", "to": "Idle",   "duration": 0.15, "hasExitTime": true, "exitTime": 1.0, "conditions": [] }
  ]
}
```
(Preserve the existing locomotion entries exactly; only ADD the `attack` param, the `Attack` state, and the last two transitions. `Attack` uses `Survey` as the placeholder clip — no real attack clip exists. `loop:false` so the one-shot holds its last frame until exit-time returns it.)

- [ ] **Step 2: Validate the JSON**

Confirm it parses (e.g. `python -c "import json;json.load(open('assets/models/Fox.animctrl.json'))"`) and that it is trackable: `git -C C:/dev/clang-examples check-ignore assets/models/Fox.animctrl.json` (must print nothing / exit 1).

- [ ] **Step 3: Commit**
```
git -C C:/dev/clang-examples add assets/models/Fox.animctrl.json
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): Fox controller Attack state (anyState trigger + exit-time return)"
```

---

## Task 6: Full build, tests, manual smoke

**Files:** none (verification only).

- [ ] **Step 1: Full build**
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: all targets link clean.

- [ ] **Step 2: Unit suites**
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_animator.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_animgraph.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_abilityroot.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
```
Expected: each prints its `All ... passed.` line. (`test_navagent` may be pre-existing RED — ignore.)

- [ ] **Step 3: Manual smoke (report results; do not auto-pass)**

PREREQUISITE: the Player entity must be a rigged Fox — Fox mesh + `SkeletonComponent` (Fox) + `AnimatorComponent` (Fox controller) + `VelocityComponent` + `PlayerComponent`. (Local `world.json` setup; the user arranges it.)

1. Launch `editor.exe`, enter Play (`InLevel`).
2. WASD → walk/run locomotion still works (no regression).
3. **LMB** → player plays the `Attack` (Survey placeholder) one-shot; **movement is locked** during the first ~60% (`kAttackRootEnd`) of the clip, then resumes; the state **auto-returns to locomotion at clip end** (exit-time) — no sticking, no param needed for return. Spamming LMB mid-attack does not re-trigger (in-flight crossfade gate).
4. Open the graph editor on the Fox controller: the `Attack` node + `anyState→Attack` + `Attack→Idle (exit-time)` links render; select `Attack→Idle` → the `has exit time` checkbox + `exit time` slider show; the AnimatorEditor live readout shows `Norm:` advancing 0→1 during Attack.

- [ ] **Step 4: Report** smoke results per step. If a step fails, debug via systematic-debugging; commit fixes under the same author. Do not mark complete on failure.

---

## Self-Review (completed during planning)

**Spec coverage:** exit-time data+evaluator+JSON+validation (T1,T2) ✓; NormalizedStateTime helper (T1) ✓; editor exit-time control + normalized readout (T3) ✓; PlayerAbilitySystem LMB→trigger (T4) ✓; cursor-read root policy + movement lock + PlayerMovementSystem guard (T4) ✓; ShouldRootMovement pure + tested (T4) ✓; Fox Attack state (T5) ✓; tests (T1,T4) ✓; smoke incl. rigged-player prereq (T6) ✓. Out-of-scope (AbilityComponent, notify-events, multi-ability, combat) correctly absent.

**Type consistency:** `SelectTransition(c, state, param, normalizedTime=0)` defined T1, called T2. `NormalizedStateTime(AnimState, stateTime, phase, clipDuration)` defined T1, used T2/T3/T4. `AnimTransition.hasExitTime/exitTime` defined T1, used T2/T3/T5-JSON. `ShouldRootMovement`/`kAttackRootEnd` defined T4, used T4-system + tested. `MovementLockedComponent` defined+used T4. `AnimatorControllerStore::Get` → `shared_ptr<const>` honored in T3/T4 (`auto`/`if (const auto ...)`).

**Verify-at-implementation (flagged inline, not placeholders):** game including `animation/AnimatorControllerStore.h` + `AnimationStore.h` (mirror existing engine-singleton includes in game.cpp — T4 Step 5); whether `MovementLockedComponent` needs any registration for the game's ECS usage (mirror `VelocityComponent` — header-instantiable, no ecs.dll change — T4 Step 5); exact include presence in AnimatorEditor.cpp (T3 Step 2). Each has a concrete fallback/mirror in-task.

---

## Execution note

T1 is pure TDD (the reusable engine primitive). T2 is a 2-line evaluator wire. T3 is editor UI. T4 is the game wiring (pure helper TDD'd; systems mechanical). T5 is the asset. No `ECS.h` change anywhere → no component-restart caveat (normal editor relaunch after an Engine/editor rebuild to smoke).
