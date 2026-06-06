# Raw Input To The Game (Boundary Piece 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Surface this tick's raw input events — including `TextInput`, which the engine currently drops — to the game, so the game can interpret text/keys itself, while the existing digested `InputStateComponent` keeps working unchanged.

**Architecture:** Extract the input-ring drain into a pure, templated `DrainInput<N>(ring, InputStateComponent&, outFrame)` in a new common header so it is unit-testable. It both digests into `InputStateComponent` (as today) AND collects every popped event in order into a caller-owned `outFrame` vector (including `TextInput`). `GameThread` owns that frame buffer (a member), fills it each tick in `DrainInputToSingleton` (which runs before `GameUpdate`), and exposes a read-only view on `GameState` (`FrameInputEvents` / `FrameInputEventCount`), valid for the current tick. Game code reaches it via the existing `g_GameState` pointer.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-community` preset), CMake. Touches `src/common/include/` (new header), `src/engine/src/threading/GameThread.{h,cpp}`, `src/game/include/game.h`, `tests/`.

**Scope:** Piece 3 of the engine/game boundary spec (`docs/superpowers/specs/2026-06-06-engine-game-boundary-design.md`). Additive — existing camera/player/menu input (which reads `InputStateComponent`) is untouched. Does NOT build any text-field UI (that's the downstream login-UI spec); this piece only makes raw text *reachable*.

---

## Background facts (verified)

- `ApplicationContext` holds `SpscRing<InputEvent, InputRingSize> InputRing` (`InputRingSize == 256`). `SpscRing` API: `bool Push(const T&)`, `bool Pop(T& out)` (`src/common/include/SpscRing.h:20,30`).
- `InputEvent` (`input.h:169-202`): `InputEventType Type` + a union — `MouseMoveEvent{X,Y}`, `MouseButtonEvent{Button,Action}`, `MouseScrollEvent{OffsetX,OffsetY}`, `KeyEvent{Key,Action,Modifier}`, `TextEvent{uint32_t Key}`. `InputEventType` enum includes `TextInput = 4` (`input.h:161-167`).
- `enum KeyCode : uint16_t` (unscoped; `KEY_A=65`, `KEY_LAST` defined). `enum InputAction : uint8_t { RELEASE=0, PRESS=1, REPEAT=2 }` (unscoped — bare `PRESS` works). `enum Button : uint8_t { ... MOUSE_BUTTON_LAST, MOUSE_BUTTON_LEFT=MOUSE_BUTTON_1 }` (unscoped).
- `InputStateComponent` (`ECS.h:111-119`): `bool KeysDown[KEY_LAST+1]; bool Pressed[KEY_LAST+1]; double MouseX,MouseY,MouseDX,MouseDY; int32_t Wheel; bool MouseDown[MOUSE_BUTTON_LAST+1]; bool MousePressed[MOUSE_BUTTON_LAST+1];`.
- Current `GameThread::DrainInputToSingleton` (`GameThread.cpp:615-654`): ensures the singleton exists; reads `prevX/prevY`; in a `ModifySingleton` lambda clears `Pressed`/`Wheel`/`MousePressed`, pops the whole ring digesting `Key`/`MouseMove`/`MouseWheel`/`MouseButton` (**`TextInput` is silently dropped**), then sets `MouseDX/DY = Mouse{X,Y} - prev`.
- Tick order in `GameThread::RunLoop` (`GameThread.cpp:451-460`): `DrainInputToSingleton(gameState)` → `m_GameLib.Update(&gameState)` (calls `GameUpdate`) → `m_Scheduler.Run(sysCtx)` (game systems). So a view set on `GameState` during the drain is valid for both `GameUpdate` and all systems this tick.
- `gameState` is a **local** in `RunLoop` (`GameThread.cpp:62`), not a member. So the frame buffer must be a `GameThread` member (stable address across ticks); the view pointer is re-pointed into it each tick.
- `g_GameState` is a file-static `GameState*` in `game.cpp:802`, set at the top of `GameUpdate`. Game systems (which receive `SystemContext`, not `GameState`) already read game state through it — so the downstream login systems read raw input via `g_GameState->FrameInputEvents` with **no `SystemContext` change** needed here.
- `GameState` layout changes here → **`GAME_API_VERSION` must bump** (currently `20` at `game.h:22`; the comment there mandates a bump on any `GameState` layout change). Rebuild game + editor and restart the editor for this change.
- `test_input.cpp` only covers the pure `RouteInputToGame` routing (links no `ecs`); it is NOT the right home for a drain test that needs `InputStateComponent`. A new target is added.

## Type/symbol contract (keep exact)

- New `src/common/include/InputDrain.h`: `template <std::size_t N> inline void DrainInput(SpscRing<InputEvent, N>& ring, InputStateComponent& s, std::vector<InputEvent>& outFrame);`
- `GameState` gains: `const InputEvent* FrameInputEvents = nullptr;` and `std::size_t FrameInputEventCount = 0;`
- `GameThread` gains member: `std::vector<InputEvent> m_FrameInput;`

---

### Task 1: Extract `DrainInput<N>` into a unit-testable common header

**Files:**
- Create: `src/common/include/InputDrain.h`
- Create: `tests/test_inputdrain.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_inputdrain.cpp`:
```cpp
#include <cstdio>
#include <vector>
#include "InputDrain.h"

static int g_Failures = 0;
#define EXPECT(c) do { if(!(c)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); ++g_Failures; } } while(0)

static InputEvent MakeKey(KeyCode k, InputAction a)   { InputEvent e(InputEventType::Key);         e.KeyEvent.Key = k; e.KeyEvent.Action = a; return e; }
static InputEvent MakeMove(double x, double y)        { InputEvent e(InputEventType::MouseMove);   e.MouseMoveEvent.X = x; e.MouseMoveEvent.Y = y; return e; }
static InputEvent MakeButton(Button b, InputAction a) { InputEvent e(InputEventType::MouseButton); e.MouseButtonEvent.Button = b; e.MouseButtonEvent.Action = a; return e; }
static InputEvent MakeText(uint32_t cp)               { InputEvent e(InputEventType::TextInput);   e.TextEvent.Key = cp; return e; }

// Digests key/mouse into InputStateComponent AND preserves every event (incl TextInput) in order.
static void T01_digest_and_raw_incl_text()
{
    SpscRing<InputEvent, 16> ring;
    InputStateComponent s;
    s.MouseX = 5.0; s.MouseY = 7.0; // previous position, for delta computation

    EXPECT(ring.Push(MakeKey(KEY_A, PRESS)));
    EXPECT(ring.Push(MakeMove(20.0, 30.0)));
    EXPECT(ring.Push(MakeText('x')));
    EXPECT(ring.Push(MakeButton(MOUSE_BUTTON_LEFT, PRESS)));

    std::vector<InputEvent> frame;
    DrainInput(ring, s, frame);

    // Digest into the convenience singleton (unchanged behavior).
    EXPECT(s.KeysDown[KEY_A]);
    EXPECT(s.Pressed[KEY_A]);
    EXPECT(s.MouseX == 20.0 && s.MouseY == 30.0);
    EXPECT(s.MouseDX == 15.0 && s.MouseDY == 23.0);
    EXPECT(s.MouseDown[MOUSE_BUTTON_LEFT]);
    EXPECT(s.MousePressed[MOUSE_BUTTON_LEFT]);

    // Raw frame: all four events, in order, with TextInput preserved (the new capability).
    EXPECT(frame.size() == 4);
    EXPECT(frame[0].Type == InputEventType::Key && frame[0].KeyEvent.Key == KEY_A);
    EXPECT(frame[1].Type == InputEventType::MouseMove);
    EXPECT(frame[2].Type == InputEventType::TextInput && frame[2].TextEvent.Key == 'x');
    EXPECT(frame[3].Type == InputEventType::MouseButton);
}

// Per-tick fields are cleared and the frame is emptied even when no events arrive.
static void T02_clears_per_tick_and_empties_frame()
{
    SpscRing<InputEvent, 16> ring;
    InputStateComponent s;
    s.Pressed[KEY_A] = true;
    s.MousePressed[MOUSE_BUTTON_LEFT] = true;
    s.Wheel = 5;
    std::vector<InputEvent> frame;
    frame.push_back(MakeText('z')); // stale leftover from a prior tick

    DrainInput(ring, s, frame); // empty ring

    EXPECT(!s.Pressed[KEY_A]);                       // per-tick "pressed" cleared
    EXPECT(!s.MousePressed[MOUSE_BUTTON_LEFT]);
    EXPECT(s.Wheel == 0);
    EXPECT(frame.empty());                           // frame cleared regardless of events
}

int main()
{
    T01_digest_and_raw_incl_text();
    T02_clears_per_tick_and_empties_frame();
    if (g_Failures == 0) { std::printf("All input-drain tests passed.\n"); return 0; }
    std::printf("%d input-drain test(s) FAILED.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 2: Add the test target to `tests/CMakeLists.txt`**

Append (mirrors `test_collision`'s block — needs `ecs` for `InputStateComponent` in `ECS.h`, and the GLM defines because `ECS.h` pulls in glm types):
```cmake
add_executable(test_inputdrain
    test_inputdrain.cpp
)

target_link_libraries(test_inputdrain PRIVATE
    CommonHeaders
    glm::glm
    ecs
)

target_include_directories(test_inputdrain PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
)

target_compile_definitions(test_inputdrain PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_inputdrain PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 3: Configure + build — expect FAIL (red)**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_inputdrain
```
Expected: build error — `Cannot open include file: 'InputDrain.h'` (header doesn't exist yet). Confirms the test targets the new API. (The first `cmake --preset` reconfigure is needed because a new target was added.)

- [ ] **Step 4: Create the header (minimal implementation)**

Create `src/common/include/InputDrain.h`:
```cpp
#pragma once
#include <cstddef>
#include <cstring>
#include <vector>

#include "input.h"
#include "ECS.h"       // InputStateComponent
#include "SpscRing.h"

// Drain ALL pending input events for this tick from `ring` into:
//  (1) `s` — the convenience InputStateComponent: per-tick fields (Pressed/MousePressed/
//            Wheel) are cleared, Key/Mouse events are digested, and mouse delta is computed
//            against `s`'s previous MouseX/MouseY. TextInput is intentionally NOT digested
//            here (the singleton has no field for it) — it is preserved raw in `outFrame`.
//  (2) `outFrame` — the raw, in-arrival-order event list INCLUDING TextInput; cleared first.
// Templated on ring capacity so GameThread's 256-deep ring and small test rings share one
// implementation. Owner-thread (GameThread) only.
template <std::size_t N>
inline void DrainInput(SpscRing<InputEvent, N>& ring, InputStateComponent& s,
                       std::vector<InputEvent>& outFrame) {
    const double prevX = s.MouseX;
    const double prevY = s.MouseY;

    std::memset(s.Pressed, 0, sizeof(s.Pressed));
    s.Wheel = 0;
    std::memset(s.MousePressed, 0, sizeof(s.MousePressed));

    outFrame.clear();
    InputEvent ev{};
    while (ring.Pop(ev)) {
        outFrame.push_back(ev); // raw, in order, including TextInput
        if (ev.Type == InputEventType::Key) {
            const int k = static_cast<int>(ev.KeyEvent.Key);
            if (k >= 0 && k <= KEY_LAST) {
                if (ev.KeyEvent.Action == PRESS || ev.KeyEvent.Action == REPEAT) s.KeysDown[k] = true;
                if (ev.KeyEvent.Action == RELEASE) s.KeysDown[k] = false;
                if (ev.KeyEvent.Action == PRESS) s.Pressed[k] = true;
            }
        } else if (ev.Type == InputEventType::MouseMove) {
            s.MouseX = ev.MouseMoveEvent.X;
            s.MouseY = ev.MouseMoveEvent.Y;
        } else if (ev.Type == InputEventType::MouseWheel) {
            s.Wheel = static_cast<int32_t>(ev.MouseScrollEvent.OffsetY);
        } else if (ev.Type == InputEventType::MouseButton) {
            const int b = static_cast<int>(ev.MouseButtonEvent.Button);
            if (b >= 0 && b <= MOUSE_BUTTON_LAST) {
                if (ev.MouseButtonEvent.Action == PRESS)   { s.MouseDown[b] = true; s.MousePressed[b] = true; }
                if (ev.MouseButtonEvent.Action == RELEASE) {  s.MouseDown[b] = false; }
            }
        }
    }
    s.MouseDX = s.MouseX - prevX;
    s.MouseDY = s.MouseY - prevY;
}
```

- [ ] **Step 5: Build + run — expect PASS (green)**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_inputdrain
./out/build/msvc-win64-vs2026-community/bin/Debug/test_inputdrain.exe
```
Expected: `All input-drain tests passed.` (exit 0).

- [ ] **Step 6: Commit**

```
git -C /c/dev/clang-examples add src/common/include/InputDrain.h tests/test_inputdrain.cpp tests/CMakeLists.txt
git -C /c/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(input): pure DrainInput<N> helper exposing raw per-tick events incl TextInput"
```
Verify `git -C /c/dev/clang-examples show HEAD --stat` lists only those three files.

---

### Task 2: Wire `DrainInput` into `GameThread` and expose the view on `GameState`

**Files:**
- Modify: `src/engine/src/threading/GameThread.h`
- Modify: `src/engine/src/threading/GameThread.cpp`
- Modify: `src/game/include/game.h`

- [ ] **Step 1: Add the frame-view fields to `GameState` and bump the API version**

In `src/game/include/game.h`:
- Ensure `InputEvent` + `std::size_t` are visible — add near the top includes (after `#include "ApplicationContext.h"`):
```cpp
#include "input.h"
#include <cstddef>
```
- Bump the version (line ~22): `#define GAME_API_VERSION 20u` → `#define GAME_API_VERSION 21u`.
- Add to the `GameState` struct (after the `World` / `WorldLoaded` / `Role` fields):
```cpp
    // Raw input events for THIS tick (incl TextInput), in arrival order, set by the engine
    // before GameUpdate; valid only for the current tick (Count 0 => none). The digested
    // convenience state still lives in the InputStateComponent singleton; this is for
    // consumers that need raw text/keys (e.g. text fields). Read via g_GameState in systems.
    const InputEvent* FrameInputEvents     = nullptr;
    std::size_t       FrameInputEventCount = 0;
```

- [ ] **Step 2: Add the frame buffer member to `GameThread`**

In `src/engine/src/threading/GameThread.h`, add to the private members (near `m_FrameInput`'s natural home, e.g. after `m_simVX`):
```cpp
    // This tick's raw input events, filled by DrainInputToSingleton and exposed read-only
    // via GameState::FrameInputEvents. A member so its storage is stable across ticks.
    std::vector<InputEvent> m_FrameInput;
```
(`<vector>` is already included; `InputEvent` is visible via the existing `#include "ApplicationContext.h"`.)

- [ ] **Step 3: Rewrite `DrainInputToSingleton` to delegate + publish the view**

In `src/engine/src/threading/GameThread.cpp`:
- Add near the top includes: `#include "InputDrain.h"`.
- Replace the entire body of `DrainInputToSingleton` (lines ~615-654) with:
```cpp
void GameThread::DrainInputToSingleton(GameState& state) {
    if (!state.World.GetSingleton<InputStateComponent>()) {
        state.World.SetSingleton(InputStateComponent{});
    }
    state.World.ModifySingleton<InputStateComponent>([&](InputStateComponent& s) {
        DrainInput(m_AppContext->InputRing, s, m_FrameInput);
    });
    // Publish this tick's raw events (incl TextInput) for GameUpdate + systems. Valid until
    // the next tick's drain re-fills m_FrameInput.
    state.FrameInputEvents     = m_FrameInput.data();
    state.FrameInputEventCount = m_FrameInput.size();
}
```
This preserves the exact digest behavior (now inside `DrainInput`) and adds the raw view. `m_AppContext->InputRing` is `SpscRing<InputEvent, 256>`, so `N` is deduced.

- [ ] **Step 4: Full build (green)**

Run:
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: clean build of all targets (`ecs`, `Engine`, `game`, `editor`, `runtime`, all `test_*`). The `game` + `editor` rebuild picks up the new `GAME_API_VERSION` and `GameState` layout. No errors.

- [ ] **Step 5: Run the input + regression suites**

Run:
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_inputdrain.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_input.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `All input-drain tests passed.`, `All input routing tests passed.`, `All ECS tests passed.`

- [ ] **Step 6: Manual input smoke (editor) — confirms wiring + no regression**

Launch `./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe`, enter Play mode, and confirm existing input still works: WASD/camera movement and mouse react (proves `InputStateComponent` digest path is intact through the refactored `DrainInput`). Close the editor. (The raw-text path itself has no UI yet — it is exercised by the downstream login-UI spec; this smoke only guards against an input regression.)

- [ ] **Step 7: Commit**

```
git -C /c/dev/clang-examples add src/engine/src/threading/GameThread.h src/engine/src/threading/GameThread.cpp src/game/include/game.h
git -C /c/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(input): expose raw per-tick input (incl TextInput) on GameState; bump GAME_API_VERSION to 21"
```
Verify `git -C /c/dev/clang-examples show HEAD --stat` lists exactly those three files.

---

## Done criteria

- `DrainInput<N>` is unit-tested (digest correctness + raw frame including `TextInput` + per-tick clearing) — Task 1.
- `GameState::FrameInputEvents`/`FrameInputEventCount` expose this tick's raw events (incl text); `GameThread` fills them before `GameUpdate`; existing `InputStateComponent` behavior unchanged; `GAME_API_VERSION` bumped to 21 — Task 2.
- Full tree builds; `test_inputdrain`/`test_input`/`test_ecs` green; editor input still works (Task 2 smoke).

## Notes for the next pieces

- The downstream login-UI spec consumes this: a `LoginUISystem` reads `g_GameState->FrameInputEvents` to feed typed characters into the focused field (Backspace/Enter via the existing `InputStateComponent` key state, printable chars via `TextInput`).
- If a future ISystem needs raw input without the `g_GameState` global, propagate the same two fields onto `SystemContext` at its construction site (`GameThread.cpp:458`). Not done here (no consumer yet; YAGNI).

## Next plans (not this one)

Piece 4 (registered serialization), Piece 5 (generic inspector editing). Each its own plan.
