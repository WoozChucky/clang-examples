# State-Gated Menu — Phase 2 (Mouse Input + Viewport Origin) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose mouse-button state to the game and make the game's mouse coordinates map into UI/viewport space, so Phase 4's menu can hit-test clicks correctly in both the editor viewport and `runtime.exe`.

**Architecture:** `InputStateComponent` gains per-button `MouseDown`/`MousePressed` arrays filled by the GameThread input drain (mirrors the existing keyboard handling). `ViewportComponent` gains `OriginX/OriginY`; the editor already tracks the scene-viewport image top-left (`m_ViewportImageMinX/Y`, used for entity picking) and publishes it via a new `ApplicationContext::SceneViewportOrigin` atomic; the GameThread writes it into `ViewportComponent`. A pure `ToUiSpace` helper subtracts the origin.

**Tech Stack:** C++23, custom ECS (`ecs.dll`), 3-thread engine, Dear ImGui editor overlay, CMake (preset `msvc-win64-vs2026-community`).

**Spec:** `docs/superpowers/specs/2026-05-25-state-gated-menu-design.md` (§3, §15 Phase 2). Builds on Phase 1 (already merged into this branch).

---

## Reference facts (verified against the codebase)

- `InputStateComponent` (`ECS.h:110-116`): `bool KeysDown[KEY_LAST+1]; bool Pressed[KEY_LAST+1]; double MouseX,MouseY,MouseDX,MouseDY; int32_t Wheel;`. `ECS.h` includes `input.h`.
- `input.h`: `enum Button : uint8_t { MOUSE_BUTTON_1=0 … MOUSE_BUTTON_8=7, MOUSE_BUTTON_LAST=MOUSE_BUTTON_8 };`; `InputAction { RELEASE=0, PRESS=1, REPEAT=2 }` (GameThread already uses **unqualified** `PRESS`/`RELEASE`); `InputEvent.MouseButtonEvent { Button Button; InputAction Action; }`; `InputEventType::MouseButton`.
- GameThread input drain (`GameThread.cpp:522-543`): clears `Pressed` + `Wheel` at the top, then a `while (InputRing.Pop(ev))` handling `Key`/`MouseMove`/`MouseWheel`. **No `MouseButton` case yet.**
- GameThread per-tick viewport sync (`GameThread.cpp:392-399`): reads `SceneViewportSize` (packed `w<<32|h`) and writes `ViewportComponent.Width/Height` + the UI ortho `glm::orthoRH_ZO(0, vw, vh, 0, -1, 1)` (top-left origin, pixel units). Singletons seeded at `GameThread.cpp:72-75`.
- `ViewportComponent` (`ECS.h:173`): `{ uint32_t Width=1920; uint32_t Height=1080; }`.
- `ApplicationContext` (`ApplicationContext.h:147`): `std::atomic<uint64_t> SceneViewportSize{0};` (0 ⇒ use OS window size; runtime never writes it).
- Editor publish site (`ImGuiRenderer.cpp:434-438`): stores `SceneViewportSize` from `m_LastViewportW/H`. The viewport image top-left is captured at `ImGuiRenderer.cpp:350-352` into `m_ViewportImageMinX/m_ViewportImageMinY` (floats, screen-space) and already used for picking (`ctx.ViewportMinX/Y`).
- `game.h`: `GAME_API_VERSION 9u` (after Phase 1).
- Test precedent that links glm: `test_picking` / `test_input` (link `glm::glm` + the three `GLM_FORCE_*` defines). `test_menu` currently links nothing (header-only).

> **ECS.h + ApplicationContext.h change here → rebuild `ecs`+`editor`+`runtime`+`game` and restart the editor once.** `GAME_API_VERSION` bumps to `10u` (Task 1) — one bump covers the whole phase since the editor is restarted once at phase end.

> **Multi-viewport caveat (informational):** `m_ViewportImageMinX/Y` is ImGui screen-space, the same value the existing entity-picking path uses to convert the mouse. For the scene docked in the main window this matches the game's window-space mouse; detached-OS-window edge cases are out of scope for this prototype (picking has the same limitation).

---

## File Structure

- Modify `src/common/include/ECS.h` — `InputStateComponent` mouse arrays; `ViewportComponent` origin fields.
- Modify `src/common/include/ApplicationContext.h` — `SceneViewportOrigin` atomic.
- Modify `src/engine/src/threading/GameThread.cpp` — drain `MouseButton`; write viewport origin.
- Modify `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` — publish viewport origin.
- Modify `src/game/include/game.h` — `GAME_API_VERSION` 9→10.
- Create `src/common/include/MenuHitTest.h` — pure `ToUiSpace` (Phase 4 adds `PointInRect` here).
- Modify `tests/test_menu.cpp` + `tests/CMakeLists.txt` — `ToUiSpace` cases (+ glm link).

---

### Task 1: Mouse-button state in InputStateComponent + GameThread drain

**Files:**
- Modify: `src/common/include/ECS.h`
- Modify: `src/engine/src/threading/GameThread.cpp`
- Modify: `src/game/include/game.h`

- [ ] **Step 1: Add the mouse-button arrays**

In `src/common/include/ECS.h`, in `struct InputStateComponent` (lines 110-116), after `int32_t Wheel = 0;` add:

```cpp
    bool    MouseDown[MOUSE_BUTTON_LAST + 1]    = {};   // held this tick
    bool    MousePressed[MOUSE_BUTTON_LAST + 1] = {};   // pressed this tick (cleared each drain)
```

(`MOUSE_BUTTON_LAST` = 7 comes from `input.h`, already included by `ECS.h`.)

- [ ] **Step 2: Clear MousePressed each tick + drain MouseButton events**

In `src/engine/src/threading/GameThread.cpp`, in the input-drain lambda (`ModifySingleton<InputStateComponent>`, ~line 522):

(a) Next to the existing per-tick clears, after `s.Wheel = 0;` (line 524), add:

```cpp
        std::memset(s.MousePressed, 0, sizeof(s.MousePressed));
```

(b) Inside the `while (m_AppContext->InputRing.Pop(ev))` loop, after the `else if (ev.Type == InputEventType::MouseWheel) { ... }` branch (line 537-539), add:

```cpp
            } else if (ev.Type == InputEventType::MouseButton) {
                const int b = static_cast<int>(ev.MouseButtonEvent.Button);
                if (b >= 0 && b <= MOUSE_BUTTON_LAST) {
                    if (ev.MouseButtonEvent.Action == PRESS)   { s.MouseDown[b] = true; s.MousePressed[b] = true; }
                    if (ev.MouseButtonEvent.Action == RELEASE) {  s.MouseDown[b] = false; }
                }
```

(This becomes the new last `else if` before the loop's closing `}`. `PRESS`/`RELEASE` are used unqualified exactly as the Key branch above does.)

- [ ] **Step 3: Bump the game API version**

In `src/game/include/game.h`, change `#define GAME_API_VERSION 9u` to `#define GAME_API_VERSION 10u`.

- [ ] **Step 4: Configure + build**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target ecs editor runtime game
```
Expected: all build with no errors.

- [ ] **Step 5: Verify the ECS test still passes (ECS.h changed)**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `All ECS tests passed.`

- [ ] **Step 6: Commit**

```bash
git add src/common/include/ECS.h src/engine/src/threading/GameThread.cpp src/game/include/game.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(input): expose mouse-button state to the game (InputStateComponent + drain)"
```

---

### Task 2: Viewport origin (publish + write into ViewportComponent)

**Files:**
- Modify: `src/common/include/ECS.h`
- Modify: `src/common/include/ApplicationContext.h`
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`
- Modify: `src/engine/src/threading/GameThread.cpp`

- [ ] **Step 1: Add origin fields to ViewportComponent**

In `src/common/include/ECS.h`, change line 173 from:

```cpp
struct ViewportComponent       { uint32_t Width = 1920; uint32_t Height = 1080; };
```

to:

```cpp
struct ViewportComponent       { uint32_t Width = 1920; uint32_t Height = 1080; uint32_t OriginX = 0; uint32_t OriginY = 0; };
```

- [ ] **Step 2: Add the SceneViewportOrigin atomic**

In `src/common/include/ApplicationContext.h`, immediately after the `SceneViewportSize` member (line 147), add:

```cpp
    // Editor scene-viewport top-left in window coords, packed (originX<<32 | originY).
    // Written by the editor overlay (RenderThread); read by the GameThread to map the
    // window-space mouse into UI/viewport space. The runtime never writes it (stays 0).
    std::atomic<uint64_t> SceneViewportOrigin{0};
```

- [ ] **Step 3: Publish the origin from the editor**

In `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`, replace the size-publish block (lines 435-438):

```cpp
        if (m_AppContext)
            m_AppContext->SceneViewportSize.store(
                (static_cast<uint64_t>(m_LastViewportW) << 32) | static_cast<uint64_t>(m_LastViewportH),
                std::memory_order_relaxed);
```

with:

```cpp
        if (m_AppContext) {
            m_AppContext->SceneViewportSize.store(
                (static_cast<uint64_t>(m_LastViewportW) << 32) | static_cast<uint64_t>(m_LastViewportH),
                std::memory_order_relaxed);
            // Scene-viewport image top-left (screen-space, same value the picker uses) so the
            // GameThread can offset the window-space mouse into UI space.
            m_AppContext->SceneViewportOrigin.store(
                (static_cast<uint64_t>(static_cast<uint32_t>(m_ViewportImageMinX)) << 32)
                    | static_cast<uint64_t>(static_cast<uint32_t>(m_ViewportImageMinY)),
                std::memory_order_relaxed);
        }
```

- [ ] **Step 4: Write the origin into ViewportComponent on the GameThread**

In `src/engine/src/threading/GameThread.cpp`, replace the viewport-size read + `ViewportComponent` write (lines 392-395):

```cpp
				const uint64_t sv = m_AppContext->SceneViewportSize.load(std::memory_order_relaxed);
				const uint32_t vw = sv ? uint32_t(sv >> 32) : m_AppContext->Settings.windowWidth;
				const uint32_t vh = sv ? uint32_t(sv & 0xffffffffu) : m_AppContext->Settings.windowHeight;
				gameState.World.ModifySingleton<ViewportComponent>([&](ViewportComponent& v){ v.Width = vw; v.Height = vh; });
```

with:

```cpp
				const uint64_t sv = m_AppContext->SceneViewportSize.load(std::memory_order_relaxed);
				const uint32_t vw = sv ? uint32_t(sv >> 32) : m_AppContext->Settings.windowWidth;
				const uint32_t vh = sv ? uint32_t(sv & 0xffffffffu) : m_AppContext->Settings.windowHeight;
				const uint64_t so = m_AppContext->SceneViewportOrigin.load(std::memory_order_relaxed);
				const uint32_t ox = uint32_t(so >> 32);
				const uint32_t oy = uint32_t(so & 0xffffffffu);
				gameState.World.ModifySingleton<ViewportComponent>([&](ViewportComponent& v){ v.Width = vw; v.Height = vh; v.OriginX = ox; v.OriginY = oy; });
```

(Runtime never writes `SceneViewportOrigin` ⇒ `ox=oy=0` ⇒ UI space == window space.)

- [ ] **Step 5: Build**

Run: `cmake --build --preset msvc-win64-vs2026-community --target ecs editor runtime game`
Expected: builds with no errors.

- [ ] **Step 6: Commit**

```bash
git add src/common/include/ECS.h src/common/include/ApplicationContext.h src/editor/src/rendering/imgui/ImGuiRenderer.cpp src/engine/src/threading/GameThread.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(input): publish editor viewport origin into ViewportComponent"
```

---

### Task 3: Pure ToUiSpace helper + test (TDD)

**Files:**
- Create: `src/common/include/MenuHitTest.h`
- Modify: `tests/test_menu.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add the failing test cases + glm to the test target**

In `tests/test_menu.cpp`, add the include near the top (after `#include "StateScope.h"`):

```cpp
#include "MenuHitTest.h" // ToUiSpace
```

Add this test function after the existing `T02_multi_state()`:

```cpp
// Window-space mouse -> UI/viewport space by subtracting the viewport origin.
static void T03_to_ui_space() {
    // Runtime: origin (0,0) -> identity.
    const glm::vec2 a = ToUiSpace(100.0, 50.0, 0u, 0u);
    EXPECT(a.x == 100.0f && a.y == 50.0f);
    // Editor: subtract the docked viewport's top-left.
    const glm::vec2 b = ToUiSpace(300.0, 220.0, 50u, 20u);
    EXPECT(b.x == 250.0f && b.y == 200.0f);
}
```

And call it in `main()` after `T02_multi_state();`:

```cpp
    T03_to_ui_space();
```

In `tests/CMakeLists.txt`, in the existing `test_menu` block, add a link-libraries line and a compile-definitions line (the target currently has neither):

```cmake
target_link_libraries(test_menu PRIVATE
    glm::glm
)

target_compile_definitions(test_menu PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)
```

(Keep the existing `add_executable`, `target_include_directories`, and `set_target_properties` for `test_menu` as-is.)

- [ ] **Step 2: Configure + build, verify it FAILS**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_menu
```
Expected: compile error — `MenuHitTest.h` not found / `ToUiSpace` undefined.

- [ ] **Step 3: Create the helper**

Create `src/common/include/MenuHitTest.h`:

```cpp
#pragma once
#include <glm/vec2.hpp>

// Convert window-space mouse coords to UI/viewport space by subtracting the viewport origin
// (top-left of the scene viewport in window coords). In runtime the origin is (0,0) so this
// is identity; in the editor it offsets by the docked viewport image. (Phase 4 adds the
// rect hit-test alongside this.)
inline glm::vec2 ToUiSpace(double mouseX, double mouseY, uint32_t originX, uint32_t originY) {
    return glm::vec2(static_cast<float>(mouseX) - static_cast<float>(originX),
                     static_cast<float>(mouseY) - static_cast<float>(originY));
}
```

- [ ] **Step 4: Build + run, verify it PASSES**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_menu
./out/build/msvc-win64-vs2026-community/bin/Debug/test_menu.exe
```
Expected: `All menu tests passed.`

- [ ] **Step 5: Commit**

```bash
git add src/common/include/MenuHitTest.h tests/test_menu.cpp tests/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(input): add pure ToUiSpace helper + test"
```

---

## Final verification

- [ ] `./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe` → `All ECS tests passed.`
- [ ] `./out/build/msvc-win64-vs2026-community/bin/Debug/test_menu.exe` → `All menu tests passed.`
- [ ] **Human smoke (restart editor — ECS.h + GAME_API changed):** Phase 2 adds no visible behavior on its own (consumed in Phase 4). Sanity only: editor + runtime still boot and behave as after Phase 1 (MainMenu gating, TAB toggle). A quick way to confirm the wiring without a menu: temporarily log in a system — *not required*; deferred to Phase 4 where clicks drive the menu. No rendering change ⇒ DX12/Vulkan low-risk.

## Self-review notes (vs. spec §3 / §15 Phase 2)

- Covers: `MouseDown[8]`/`MousePressed[8]` on `InputStateComponent` + GameThread `MouseButton` drain (PRESS→Down+Pressed, RELEASE→clear, MousePressed cleared each tick); `ViewportComponent.OriginX/OriginY` + `SceneViewportOrigin` atomic + editor publish (from the existing `m_ViewportImageMinX/Y`) + GameThread write; pure `ToUiSpace` helper + test. `GAME_API_VERSION` bump.
- Deferred (correctly absent): `StateScopeComponent`/`UIRectComponent`/`MenuButtonComponent`/`Actions.h` (Phase 3-4); the actual click hit-test (`PointInRect`) + menu interaction wiring (Phase 4). `ToUiSpace` lands now with its test but isn't consumed until Phase 4.
- `MenuHitTest.h` is created now (holds `ToUiSpace`); Phase 4 will extend it with `PointInRect` + the click-latch helper.
- Array size uses `MOUSE_BUTTON_LAST + 1` (= 8), matching the `KEY_LAST + 1` keyboard pattern.
