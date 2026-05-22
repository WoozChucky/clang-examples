# Editor Input Routing — Design

**Date:** 2026-05-22
**Status:** Approved (design); pending implementation plan.
**Branch:** stacking on `main`.

## Goal

Separate in-game input from the editor's ImGui UI so a single physical input (mouse wheel,
click, key) is no longer handled by both. Today every input event is fanned out unconditionally
to both the game and ImGui, so e.g. the wheel zooms the scene **and** scrolls a hovered panel.

End state — two modes:
- **Cursor locked** (`GLFW_CURSOR_DISABLED`, the existing `T`-toggle play/FPS mode): the game owns
  ALL input; viewport gating is bypassed.
- **Cursor normal** (editor mode): the game receives **mouse** input only when the **Viewport**
  panel is hovered (and no gizmo is being dragged), and **keyboard** input only when the Viewport
  is focused (and no ImGui text field is active). ImGui always receives every event so panels work.

`runtime.exe` (no ImGui overlay) is unaffected — the game keeps getting all input.

## Background (verified)

- **Fan-out (the choke point):** the five input-producing `src/engine/src/threading/PlatformThread.cpp`
  GLFW callbacks (`OnCursorPositionCallback` :94, `OnMouseButtonCallback` :112,
  `OnMouseWheelCallback` :130, `OnKeyCallback` :148, `OnTextInputCallback` :187) each build an
  `InputEvent` and push it to **both** `m_AppContext->InputRing` (→ GameThread) **and**
  `m_AppContext->ImGuiInputRing` (→ ImGui on the render thread), unconditionally. (`OnWindowResizeCallback`
  :204 is separate — it pushes a `RendererCommand` to `PRCommandRing`, not an `InputEvent`.) These
  callbacks run on the platform/main thread during `glfwPollEvents`.
- **Cursor-lock toggle:** `OnKeyCallback` :166-176 toggles `GLFW_CURSOR` between `NORMAL` and
  `DISABLED` on `T` (currently labelled "Example"). This is the play/FPS "game owns input" signal.
- **Game consumes the game ring directly:** the hot-reloaded `Game.dll` reads input from the
  game `InputRing` (via `GameState::PlatformInput`); the camera/zoom logic lives in
  `src/game/src/game.cpp`. Gating at the fan-out means **game code is untouched**.
- **ImGui input + capture state (render thread):** `ImGuiRenderer::ProcessInputEvents`
  (`src/editor/src/rendering/imgui/ImGuiRenderer.cpp` :478) drains `ImGuiInputRing` into ImGui
  `io`. After `NewFrame` + widgets, `io.WantTextInput`/`io.WantCaptureMouse` are valid. The
  `"Viewport"` window is drawn in `Render` (~:310) — `ImGui::IsWindowHovered()`/`IsWindowFocused()`
  are available inside its `Begin` block. `ImGuizmo::IsUsing()` reports an active gizmo drag.
- **Why `WantCaptureMouse` alone is insufficient:** the scene now lives inside the `"Viewport"`
  ImGui window, so `io.WantCaptureMouse` is true whenever the mouse is over the viewport — gating
  the game on `!WantCaptureMouse` would starve the scene of input. Editors instead gate on the
  **viewport panel being the input target** (hovered/focused), which is what this design does.
- **Cross-thread pattern precedent:** `ApplicationContext` already carries cross-thread editor
  state as atomics (e.g. `SceneViewportSize`); the same render-thread-writes / other-thread-reads
  pattern applies here.

## Scope

**In scope:** two `std::atomic<bool>` flags on `ApplicationContext`; the editor overlay publishing
them each frame; a pure `RouteInputToGame(...)` decision function; `PlatformThread` tracking the
cursor-lock state and gating the game-ring push per event via that function; a unit test for the
function.

**Out of scope / unchanged:** `Game.dll`/game input or camera logic, the ImGui ring (ImGui keeps
receiving all events), `Engine` render/ECS, `runtime` behavior, `GAME_API_VERSION`. **Not doing
(YAGNI):** the mouse-drag-continuation latch (press in viewport, drag outside the panel) — current
editor interactions are wheel/click and FPS-drag uses cursor-lock (which bypasses gating); add it
later if editor click-drag camera is introduced. Not gating ImGui itself in play mode (harmless).

## Design

### 1. Shared flags — `ApplicationContext`

```cpp
// Editor input routing (RenderThread writes, PlatformThread reads). Default true so the runtime
// and the editor's first frame route all input to the game; the editor overlay overwrites these
// every frame based on the Viewport panel's hover/focus + ImGui capture state.
std::atomic<bool> GameAcceptsMouse{true};
std::atomic<bool> GameAcceptsKeyboard{true};
```

### 2. Pure decision function — `src/common/include/InputRouting.h` (new, header-only)

```cpp
#pragma once
#include "Input.h"   // InputEventType

// Decides whether an input event should be routed to the game ring (in addition to ImGui, which
// always receives it). cursorLocked => play/FPS mode: the game owns everything. Otherwise mouse
// events are gated by acceptsMouse (Viewport hovered & no gizmo drag) and keyboard/text events by
// acceptsKeyboard (Viewport focused & no text field); non-input events (resize) always pass.
inline bool RouteInputToGame(InputEventType type, bool cursorLocked,
                             bool acceptsMouse, bool acceptsKeyboard)
{
    if (cursorLocked) return true;
    switch (type) {
        case InputEventType::MouseMove:
        case InputEventType::MouseButton:
        case InputEventType::MouseWheel:
            return acceptsMouse;
        case InputEventType::Key:
        case InputEventType::TextInput:
            return acceptsKeyboard;
        default:
            return true; // defensive fallback for any future InputEventType
    }
}
```
(`InputEventType` (in `src/common/include/Input.h`) has exactly five values — `MouseMove`,
`MouseButton`, `MouseWheel`, `Key`, `TextInput` — all covered above. Window resize is NOT an input
event: `OnWindowResizeCallback` pushes a `RendererCommand` to `PRCommandRing`, so it is unaffected
by this gate. The `default` branch only guards future enumerators.)

### 3. `PlatformThread` — track cursor lock + gate the game-ring push

- Add a member `bool m_CursorLocked = false;` (cursor starts `NORMAL`). In the `T`-key handler
  (`OnKeyCallback` :166-176) set `m_CursorLocked` to match the new mode when toggling (replace the
  `glfwGetInputMode` re-query with the tracked bool, or set the bool alongside the existing
  `glfwSetInputMode` calls).
- In each of the 5 input callbacks, replace the unconditional
  `m_AppContext->InputRing.Push(ev);` with:
  ```cpp
  if (RouteInputToGame(ev.Type, m_CursorLocked,
                       m_AppContext->GameAcceptsMouse.load(std::memory_order_relaxed),
                       m_AppContext->GameAcceptsKeyboard.load(std::memory_order_relaxed)))
  {
      if (!m_AppContext->InputRing.Push(ev))
          SM_WARN("InputRing full, dropping evt");
  }
  ```
  The `ImGuiInputRing.Push(ev)` call stays unconditional in every callback (ImGui always gets the
  event). `OnWindowResizeCallback` is not an input-ring callback (it posts a `RendererCommand`), so
  it is untouched.

### 4. Editor overlay publishes the flags — `ImGuiRenderer::Render`

Inside the `"Viewport"` window `Begin` block, capture hover/focus into members:
```cpp
m_ViewportHovered = ImGui::IsWindowHovered();
m_ViewportFocused = ImGui::IsWindowFocused();
```
At the END of `Render` (after all panels + widgets, so `io.WantTextInput` is valid), publish:
```cpp
if (m_AppContext) {
    m_AppContext->GameAcceptsMouse.store(m_ViewportHovered && !ImGuizmo::IsUsing(),
                                         std::memory_order_relaxed);
    m_AppContext->GameAcceptsKeyboard.store(m_ViewportFocused && !ImGui::GetIO().WantTextInput,
                                            std::memory_order_relaxed);
}
```
New `ImGuiRenderer` members: `bool m_ViewportHovered = false; bool m_ViewportFocused = false;`.
The editor overlay does **not** consult cursor mode — the `PlatformThread` cursor-lock override
handles play mode; the overlay only reports "does the Viewport want this input."

## Data flow

1. RenderThread (editor): `ImGuiRenderer::Render` draws the Viewport, records hovered/focused,
   and at frame end stores `GameAcceptsMouse`/`GameAcceptsKeyboard` into `ApplicationContext`.
2. PlatformThread: each GLFW callback calls `RouteInputToGame(type, m_CursorLocked, accepts...)`;
   if true → push to `InputRing` (game). Always push to `ImGuiInputRing`.
3. GameThread / `Game.dll`: reads `InputRing` as before — now it only sees events meant for it.

Result: wheel over a panel scrolls only the panel; wheel over the viewport zooms only the scene;
typing in a field doesn't drive the game; dragging a gizmo doesn't also move the camera; `T`
(cursor lock) gives the game everything (FPS mode). ~1-frame latency on the flags (render→platform),
identical to other cross-thread editor signals — acceptable.

## Build / verification

Build preset `msvc-win64-vs2026-community`. No `GAME_API_VERSION` bump; `Game.dll`/engine
render/ECS untouched.

- **Unit test** (`RouteInputToGame` is pure → testable in isolation): add cases — cursorLocked
  routes every type to the game; cursor-normal mouse types follow `acceptsMouse`; key/text types
  follow `acceptsKeyboard`. Mirror the repo's `test_frustum` target style (a small `test_input`
  exe linking `CommonHeaders` — implementer's call based on what `InputRouting.h`/`Input.h` need to
  compile standalone).
- `editor` + `runtime` build clean; `test_ecs`/`test_alloc`/`test_frustum` stay green.
- **GUI smoke (user):** wheel over the Render Stats / Memory / Inspector panels scrolls only the
  panel (scene does NOT zoom); wheel over the Viewport zooms the scene only; click/scroll on a
  panel doesn't move the camera; typing in a text field (e.g. a load-path / numeric input) doesn't
  drive the game; dragging a gizmo doesn't also orbit the camera; press `T` → cursor locks and the
  game receives all input (FPS look) regardless of panel hover; press `T` again → editor gating
  resumes. `runtime.exe` responds to all input exactly as before.

## Risks

- **`WantCaptureMouse` misuse** → starves the viewport. Mitigated by gating on viewport
  hover/focus (not `WantCaptureMouse`), which is correct for a docked-image viewport.
- **1-frame flag latency** → a single frame of "wrong" routing when crossing a panel boundary.
  Negligible and standard; the user won't perceive it.
- **Stuck camera drag** if editor click-drag camera is later added (press in viewport, drag out →
  `GameAcceptsMouse` flips false mid-drag). Out of scope now (no editor click-drag camera; FPS
  drag uses cursor-lock). The drag-continuation latch is the documented follow-up.
- **Cursor-lock state desync** if the `T` handler and `m_CursorLocked` diverge → mitigated by
  setting the bool in the same handler that calls `glfwSetInputMode` (single source).
- **`InputEventType` enumerator names** differ from assumed → caught at compile; the implementer
  confirms them in the input header before writing `RouteInputToGame`.
