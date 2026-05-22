# Editor Input Routing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Route input so the editor's ImGui and the game don't both react to one event — game gets mouse only when the Viewport is hovered (no gizmo drag) and keyboard only when it's focused (no text field); cursor-lock (T) gives the game everything; ImGui always gets events.

**Architecture:** A pure `RouteInputToGame(...)` decides per-event whether to push to the game `InputRing`; `PlatformThread` calls it at fan-out (always pushing to the ImGui ring). The editor overlay publishes two `atomic<bool>` flags on `ApplicationContext` each frame from the Viewport's hover/focus + ImGui capture state. Game code is untouched; runtime is unaffected (flags default true).

**Tech Stack:** C++23, GLFW (platform input), Dear ImGui + ImGuizmo, lock-free SPSC rings, CMake presets.

**Spec:** `docs/superpowers/specs/2026-05-22-editor-input-routing-design.md`

**Conventions (every task):**
- Build preset `msvc-win64-vs2026-community`; build dir `out/build/msvc-win64-vs2026-community`; exes in `bin/Debug/`.
- New `.cpp`/target → `cmake --preset msvc-win64-vs2026-community` before building.
- No `GAME_API_VERSION` bump; `Game.dll`/engine ECS/render untouched.
- Commit author is the repo default (`Nuno Silva <nuno.levezinho@live.com.pt>`) — plain `git commit`. Never stage `.claude/`. Stage only the files each step names. After each commit, `git log -1 --format='%an <%ae>'` must show the personal email.

---

### Task 1: `RouteInputToGame` pure decision function + unit test

**Files:**
- Create: `src/common/include/InputRouting.h`
- Create: `tests/test_input.cpp`
- Modify: `tests/CMakeLists.txt` (append a `test_input` target)

- [ ] **Step 1: Write the failing test** — Create `tests/test_input.cpp` (self-contained, like `test_frustum.cpp`):

```cpp
#include <cstdio>
#include "Input.h"
#include "InputRouting.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static void T00_smoke() { EXPECT(1 + 1 == 2); }

// cursorLocked => everything routes to the game regardless of the accept flags.
static void T01_cursor_locked_routes_all()
{
    EXPECT(RouteInputToGame(InputEventType::MouseMove,   true, false, false) == true);
    EXPECT(RouteInputToGame(InputEventType::MouseButton, true, false, false) == true);
    EXPECT(RouteInputToGame(InputEventType::MouseWheel,  true, false, false) == true);
    EXPECT(RouteInputToGame(InputEventType::Key,         true, false, false) == true);
    EXPECT(RouteInputToGame(InputEventType::TextInput,   true, false, false) == true);
}

// cursor normal: mouse-type events follow acceptsMouse.
static void T02_editor_mouse_follows_acceptsMouse()
{
    EXPECT(RouteInputToGame(InputEventType::MouseMove,   false, true,  false) == true);
    EXPECT(RouteInputToGame(InputEventType::MouseButton, false, true,  false) == true);
    EXPECT(RouteInputToGame(InputEventType::MouseWheel,  false, true,  false) == true);
    EXPECT(RouteInputToGame(InputEventType::MouseMove,   false, false, true)  == false);
    EXPECT(RouteInputToGame(InputEventType::MouseWheel,  false, false, true)  == false);
}

// cursor normal: key/text events follow acceptsKeyboard.
static void T03_editor_keyboard_follows_acceptsKeyboard()
{
    EXPECT(RouteInputToGame(InputEventType::Key,       false, false, true)  == true);
    EXPECT(RouteInputToGame(InputEventType::TextInput, false, false, true)  == true);
    EXPECT(RouteInputToGame(InputEventType::Key,       false, true,  false) == false);
    EXPECT(RouteInputToGame(InputEventType::TextInput, false, true,  false) == false);
}

int main()
{
    T00_smoke();
    T01_cursor_locked_routes_all();
    T02_editor_mouse_follows_acceptsMouse();
    T03_editor_keyboard_follows_acceptsKeyboard();

    if (g_Failures == 0) { std::printf("All input routing tests passed.\n"); return 0; }
    std::printf("%d input routing test(s) FAILED.\n", g_Failures);
    return 1;
}
```
(`InputEventType` (in `src/common/include/Input.h`) has exactly five values: `MouseMove=0, MouseButton=1, MouseWheel=2, Key=3, TextInput=4`. Window resize is NOT an input event — it goes through `PRCommandRing` as a `RendererCommand` — so there is no resize enumerator to test. The switch in `RouteInputToGame` covers all five values; its `default` is a defensive fallback for any future enumerator.)

- [ ] **Step 2: Add the `test_input` target** — append to `tests/CMakeLists.txt`:

```cmake
add_executable(test_input
    test_input.cpp
)

target_link_libraries(test_input PRIVATE
    CommonHeaders
    glm::glm
)

target_compile_definitions(test_input PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_input PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```
(`CommonHeaders` puts `src/common/include` on the include path so `Input.h`/`InputRouting.h` resolve. `glm::glm` + the GLM defines are belt-and-suspenders in case `Input.h` transitively includes glm; harmless otherwise.)

- [ ] **Step 3: Reconfigure + build → expect FAIL (header missing)**

```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target test_input
```
Expected: build error — `Cannot open include file: 'InputRouting.h'`.

- [ ] **Step 4: Create `src/common/include/InputRouting.h`**

```cpp
#pragma once
#include "Input.h" // InputEventType

// Decides whether an input event should be routed to the game ring (in addition to ImGui, which
// always receives it). cursorLocked => play/FPS mode: the game owns everything. Otherwise mouse
// events are gated by acceptsMouse (Viewport hovered & no gizmo drag) and keyboard/text events by
// acceptsKeyboard (Viewport focused & no text field). The default branch is a defensive fallback
// for any future InputEventType (the current five values are all covered above).
inline bool RouteInputToGame(InputEventType type, bool cursorLocked,
                             bool acceptsMouse, bool acceptsKeyboard)
{
    if (cursorLocked)
        return true;
    switch (type) {
        case InputEventType::MouseMove:
        case InputEventType::MouseButton:
        case InputEventType::MouseWheel:
            return acceptsMouse;
        case InputEventType::Key:
        case InputEventType::TextInput:
            return acceptsKeyboard;
        default:
            return true;
    }
}
```

- [ ] **Step 5: Build + run → expect PASS**

```
cmake --build out/build/msvc-win64-vs2026-community --target test_input
./out/build/msvc-win64-vs2026-community/bin/Debug/test_input.exe
```
Expected: `All input routing tests passed.` (exit 0).

- [ ] **Step 6: Commit**

```bash
git add src/common/include/InputRouting.h tests/test_input.cpp tests/CMakeLists.txt
git commit -m "feat: add RouteInputToGame input-routing predicate + test_input"
```

---

### Task 2: `ApplicationContext` flags + `PlatformThread` gating

After this task the gate is live but behavior is unchanged: nobody publishes the flags yet, so they stay `true` and the game still receives all input (Task 3 activates gating). The cursor-lock path already routes everything to the game.

**Files:**
- Modify: `src/common/include/ApplicationContext.h` (add two atomics to `struct ApplicationContext`)
- Modify: `src/engine/src/threading/PlatformThread.h` (add `m_CursorLocked` member)
- Modify: `src/engine/src/threading/PlatformThread.cpp` (include InputRouting.h; set `m_CursorLocked` in the T handler; gate the 5 game-ring pushes)

- [ ] **Step 1: Add the flags to `ApplicationContext`** — in `src/common/include/ApplicationContext.h`, after the `std::atomic<uint64_t> SceneViewportSize{0};` member (added in a prior feature), add:
```cpp

    // Editor input routing (RenderThread writes, PlatformThread reads). Default true so the
    // runtime and the editor's first frame route all input to the game; the editor overlay
    // overwrites these every frame from the Viewport panel's hover/focus + ImGui capture state.
    std::atomic<bool> GameAcceptsMouse{true};
    std::atomic<bool> GameAcceptsKeyboard{true};
```

- [ ] **Step 2: Add the cursor-lock member to `PlatformThread.h`** — in the private members of `class PlatformThread` (`src/engine/src/threading/PlatformThread.h`), add:
```cpp
    bool m_CursorLocked = false; // GLFW_CURSOR_DISABLED (play/FPS mode) -> game owns all input
```

- [ ] **Step 3: Track cursor lock in the T handler** — in `src/engine/src/threading/PlatformThread.cpp`, the `T`-key block (currently lines ~166-176) reads the mode via `glfwGetInputMode` and toggles it. Replace it so it also updates `m_CursorLocked`:
```cpp
    if (key == GLFW_KEY_T && action == GLFW_PRESS) {
        // Toggle cursor lock: DISABLED = play/FPS mode (game owns all input), NORMAL = editor.
        m_CursorLocked = !m_CursorLocked;
        glfwSetInputMode(m_Window, GLFW_CURSOR,
                         m_CursorLocked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        SM_TRACE(m_CursorLocked ? "Cursor disabled" : "Cursor normal");
    }
```

- [ ] **Step 4: Add the include** — near the top of `PlatformThread.cpp`, with the other includes, add:
```cpp
#include "InputRouting.h"
```

- [ ] **Step 5: Gate the game-ring push in all 5 input callbacks**

In each of `OnCursorPositionCallback`, `OnMouseButtonCallback`, `OnMouseWheelCallback`,
`OnKeyCallback`, `OnTextInputCallback`, the event is built into a local `ev`, then pushed to the
game ring with:
```cpp
    // Push to game thread
    if (!m_AppContext->InputRing.Push(ev)) {
        SM_WARN("InputRing full, dropping evt");
    }
```
Replace **that game-ring push block** (in all 5 callbacks) with the gated version:
```cpp
    // Push to game thread (gated: editor routes scene input only when the Viewport wants it;
    // cursor-lock/play mode routes everything; ImGui always gets the event below).
    if (RouteInputToGame(ev.Type, m_CursorLocked,
                         m_AppContext->GameAcceptsMouse.load(std::memory_order_relaxed),
                         m_AppContext->GameAcceptsKeyboard.load(std::memory_order_relaxed))) {
        if (!m_AppContext->InputRing.Push(ev)) {
            SM_WARN("InputRing full, dropping evt");
        }
    }
```
Leave the `ImGuiInputRing.Push(ev)` block in each callback UNCHANGED (ImGui always receives the
event). `OnWindowResizeCallback` is NOT one of these five — it pushes a `RendererCommand` to
`PRCommandRing`, not an `InputEvent`, so it is untouched. (Do NOT gate the ImGui ring anywhere.)

- [ ] **Step 6: Build engine + editor + runtime, run tests**

```
cmake --build out/build/msvc-win64-vs2026-community --target Engine
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs
cmake --build out/build/msvc-win64-vs2026-community --target test_alloc
cmake --build out/build/msvc-win64-vs2026-community --target test_input
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_input.exe
```
Expected: all build clean; `All ECS tests passed.`, `All allocator tests passed.`, `All input routing tests passed.` Behavior unchanged (flags default true → game still gets all input; cursor-lock routes all).

- [ ] **Step 7: Commit**

```bash
git add src/common/include/ApplicationContext.h src/engine/src/threading/PlatformThread.h src/engine/src/threading/PlatformThread.cpp
git commit -m "feat: gate game-ring input via RouteInputToGame + cursor-lock tracking"
```

---

### Task 3: Editor overlay publishes the routing flags

Activates gating: the editor reports whether the Viewport wants the input.

**Files:**
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.h` (two members)
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` (capture hover/focus; publish flags)

- [ ] **Step 1: Add members to `ImGuiRenderer.h`** — in the private members (next to the Viewport rect members `m_LastViewportW`/`m_ViewportImageMinX` etc.), add:
```cpp
    bool m_ViewportHovered = false;  // Viewport panel hovered this frame (mouse routing)
    bool m_ViewportFocused = false;  // Viewport panel focused this frame (keyboard routing)
```

- [ ] **Step 2: Capture hover/focus inside the Viewport window** — in `ImGuiRenderer::Render`, inside the `if (ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoMove))` block (currently ~lines 310-324), after `m_ViewportDrawList = ImGui::GetWindowDrawList();`, add:
```cpp
            m_ViewportHovered = ImGui::IsWindowHovered();
            m_ViewportFocused = ImGui::IsWindowFocused();
```
Also set them to `false` when the Begin returns false (window collapsed/hidden): initialize before the `if`:
```cpp
        m_ViewportHovered = false;
        m_ViewportFocused = false;
```
(Place this init right next to the existing `m_ViewportDrawList = nullptr;` line that precedes the `if (ImGui::Begin("Viewport"...))`.)

- [ ] **Step 3: Publish the routing flags at the end of `Render`** — just before `ImGui::PopFont();` near the end of `Render` (after all panels have drawn, so `io.WantTextInput` reflects any active text field this frame), add:
```cpp
        if (m_AppContext) {
            m_AppContext->GameAcceptsMouse.store(m_ViewportHovered && !ImGuizmo::IsUsing(),
                                                 std::memory_order_relaxed);
            m_AppContext->GameAcceptsKeyboard.store(m_ViewportFocused && !ImGui::GetIO().WantTextInput,
                                                    std::memory_order_relaxed);
        }
```
(`ImGuizmo.h` and ImGui are already included in this file. If a local `ImGuiIO& io` is already in scope at that point you may use it instead of `ImGui::GetIO()`.)

- [ ] **Step 4: Build editor, run tests**

```
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs
cmake --build out/build/msvc-win64-vs2026-community --target test_alloc
cmake --build out/build/msvc-win64-vs2026-community --target test_input
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_input.exe
```
Expected: clean build; all three test exes pass.

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/rendering/imgui/ImGuiRenderer.h src/editor/src/rendering/imgui/ImGuiRenderer.cpp
git commit -m "feat: publish editor input-routing flags from the Viewport panel state"
```

---

## Final verification (after all tasks)

- [ ] Full build: `cmake --build out/build/msvc-win64-vs2026-community` — all targets green.
- [ ] `test_ecs` / `test_alloc` / `test_frustum` / `test_input` all print their `All ... passed.` lines.
- [ ] **GUI smoke (user-run, surface to the user — do not self-approve):**
  - Mouse wheel over the Render Stats / Memory / Inspector panels scrolls only the panel — the scene does NOT zoom.
  - Mouse wheel over the Viewport zooms only the scene.
  - Clicking/scrolling a panel doesn't move the camera; typing in a numeric/text field doesn't drive the game.
  - Dragging a gizmo doesn't also orbit/zoom the camera.
  - Press `T`: cursor locks, the game receives all input (FPS look) regardless of panel hover; press `T` again: editor gating resumes.
  - `runtime.exe`: responds to all input exactly as before (no panels; flags stay default true).

## Notes / non-goals
- No `GAME_API_VERSION` bump; `Game.dll`/engine ECS/render untouched. Game code never changes — gating is at the engine input boundary.
- 1-frame latency on the flags (render→platform), as with other cross-thread editor signals.
- Not implemented (YAGNI): mouse-drag-continuation latch (press in viewport, drag outside the panel) — current editor interactions are wheel/click and FPS-drag uses cursor-lock; revisit if editor click-drag camera is added. ImGui is not gated in play mode (harmless).
