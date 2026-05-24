# Editor Camera (Free-Look Viewport Camera) — Design

**Date:** 2026-05-24
**Status:** Approved (design); pending implementation plan.
**Branch:** stacking on `main` (implementation on `editor-camera`).

## Goal

Give the editor its own **free-look viewport camera**, independent of the game camera, for
navigating the scene while editing. An **Edit/Play toggle** decides which camera drives the
viewport: in **Edit** mode the editor camera flies freely (and picking/outline use it); in **Play**
mode the game's `WorldCameraComponent` drives the view, exactly as today.

The editor camera is **editor-owned** (lives on the RenderThread, in the ImGui overlay layer), so it
is always available to navigate regardless of what the game does with its own camera, it survives
`Game.dll` hot-reloads, and it adds nothing to the game ECS.

## Background (verified)

- **The camera you already fly is the game camera.** `FreeLookCameraSystem` lives in the game
  (`src/game/src/game.cpp:87-140`), runs on the GameThread (`SystemPhase::Simulation`), reads
  `InputStateComponent` (WASD/mouse/wheel) + `ViewportComponent` (aspect) and writes the
  `WorldCameraComponent` singleton each tick. It is part of the game sim, not the editor.
- **One camera singleton feeds all world rendering.** `WorldCameraComponent`
  (`src/common/include/ECS.h:117-121`: `glm::mat4 View; glm::mat4 Projection; glm::vec3 Position;`,
  singleton). Read by the three world-space passes:
  - `MeshRenderPass.cpp:361`
  - `PrimitiveRenderPass.cpp:264`
  - `OutlineRenderPass.cpp:114`
  Each does `world->GetSingleton<WorldCameraComponent>()`, falling back to identity `V`/`P` when
  null. `UiRenderPass.cpp:280` reads the orthographic `UICameraComponent` instead — **unaffected**.
- **All four passes already hold a `Renderer* m_Renderer`** (set in `Initialize`), and
  `m_Renderer->GetAppContext()` is available (`OutlineRenderPass.cpp:98` already uses it for
  `SelectedEntity`). So passes can read a Renderer-resolved camera with **no interface change**.
- **Renderer pass loop:** `Renderer::Render` runs the scene passes (`Renderer.cpp:203-208`), then
  `m_Overlay->Render(...)` (`Renderer.cpp:221`) — the editor overlay runs **after** the scene
  passes. So a camera the overlay computes this frame applies **next** frame → **1-frame lag**, the
  same accepted lag as `SelectedEntity`→`OutlineRenderPass`.
- **Picking** (`src/editor/src/rendering/imgui/ViewportPicker.cpp:11-51`): `PickEntity` builds a ray
  via `ScreenPointToRay` (`src/common/include/Picking.h`) from `ctx.World`'s `WorldCameraComponent`
  `View`/`Projection` + the viewport rect in `EditorContext` (`ViewportMinX/Y`, `ViewportW/H`). To
  stay consistent with the rendered image, picking must use the **same** camera the passes used.
- **Editor↔engine channels** (`src/common/include/ApplicationContext.h`): the editor (RenderThread)
  already publishes `SceneViewportSize` (:145), `GameAcceptsMouse`/`GameAcceptsKeyboard` (:150-151),
  `SelectedEntity` (:161). `Seqlock<T>` (`src/common/include/Seqlock.h`) is single-writer,
  lock-free, requires a trivially-copyable `T`. **No camera-override channel exists yet.**
- **Input gating** (`PlatformThread.cpp:100-101`): PlatformThread enqueues input to the game ring
  only when `GameAcceptsMouse`/`GameAcceptsKeyboard` are true. The overlay sets these from viewport
  hover/focus (`ImGuiRenderer.cpp:369-374`). The editor itself reads mouse/keyboard via **ImGui IO**
  on the RenderThread — so the editor camera can be driven entirely from ImGui input, no game ring.
- **EditorContext** (`src/editor/src/rendering/imgui/EditorContext.h`): per-frame struct passed to
  panels; carries `App`, `World`, `MeshSys`, the viewport rect. No glm/camera fields yet.

## Scope

**In scope:**
1. A camera-override channel in `ApplicationContext`: `std::atomic<bool> EditorCameraActive` + a
   `Seqlock<CameraView>` (`CameraView = {mat4 View; mat4 Projection; vec3 Position;}`).
2. `Renderer` resolves the active camera once per frame (override-or-snapshot) and exposes it; the 3
   world passes read it instead of `GetSingleton<WorldCameraComponent>()` directly.
3. An editor-owned `EditorCamera` controller (pure math, unit-tested) + Unity-style fly controls.
4. Edit/Play toggle (toolbar button + hotkey) in the editor; input-routing + channel writes.
5. Picking uses the editor camera when Edit mode is active.
6. A `test_editorcam` unit-test target for the camera math.

**Out of scope / non-goals:** editor-camera pose persistence across sessions (session-only,
in-memory; resets on editor restart); pausing the sim in Edit mode (sim **keeps ticking** — mode
controls only camera + input routing; the existing pause is independent); FOV animation; multiple
editor viewports/cameras; gizmo/grid changes; saving the editor camera into `world.json`. **No
`GAME_API_VERSION` bump** (no `GameState`/export/ECS-component change — the editor camera is editor
state, not an ECS component).

## Design

### 1. Camera-override channel (`ApplicationContext.h`)

Add a small trivially-copyable view struct and two members:

```cpp
struct CameraView {
    glm::mat4 View{1.0f};
    glm::mat4 Projection{1.0f};
    glm::vec3 Position{0.0f};
};
// ... in ApplicationContext:
// Editor free-look camera override (editor only). The overlay (RenderThread) writes both each
// frame; Renderer (RenderThread, earlier in the next frame) reads them -> 1-frame lag, like
// SelectedEntity. Runtime has no overlay, never writes them, so EditorCameraActive stays false
// and rendering uses the game's WorldCameraComponent unchanged.
std::atomic<bool>   EditorCameraActive{false};
Seqlock<CameraView> EditorCamera{};
```

`CameraView` is trivially copyable (glm `mat4`/`vec3` are PODs; default member initializers don't
affect that), satisfying the `Seqlock` `static_assert`.

### 2. Renderer resolves the active camera

In `Renderer::Render`, **before** the pass loop (`Renderer.cpp:203`), resolve once into a member
`CameraView m_ActiveCamera;` and expose `const CameraView& GetActiveCamera() const`:

```cpp
CameraView active{}; // identity V/P, zero pos — matches the passes' current null fallback
if (const ApplicationContext* ctx = GetAppContext();
    ctx && ctx->EditorCameraActive.load(std::memory_order_relaxed)) {
    active = ctx->EditorCamera.load();
} else if (world) {
    if (const auto* cam = world->GetSingleton<WorldCameraComponent>())
        active = { cam->View, cam->Projection, cam->Position };
}
m_ActiveCamera = active;
```

Then the three world passes replace their `world->GetSingleton<WorldCameraComponent>()` camera block
with `const CameraView& cam = m_Renderer->GetActiveCamera();`, reading the fields each already uses:

```cpp
// MeshRenderPass / PrimitiveRenderPass (need camPos for frustum culling):
V = cam.View; P = cam.Projection; camPos = cam.Position;
// OutlineRenderPass (View/Projection only — has no camPos):
V = cam.View; P = cam.Projection;
```

`UiRenderPass` is untouched (uses `UICameraComponent`).

**Runtime invariant (hard requirement):** when `EditorCameraActive` is false (always, in
`runtime.exe`), `m_ActiveCamera` equals the snapshot's `WorldCameraComponent` (or identity when
absent) — byte-identical to today. This is the one shared-code change; it must not regress the
no-override path.

### 3. `EditorCamera` controller (editor-owned, pure math)

New `src/editor/src/rendering/EditorCamera.{h,cpp}`. Holds session-only state:

```cpp
glm::vec3 Position{0.0f, 5.0f, 10.0f};  // matches the game free-look default
float Yaw   = 0.0f;                     // radians, around +Y
float Pitch = 0.0f;                     // radians, clamped to +/-89 deg
float Fov   = glm::radians(60.0f);
float FlySpeed = 7.5f;                  // units/sec, wheel-adjustable while RMB held
```

A per-frame input struct (filled by the overlay from ImGui IO) and an `Update`:

```cpp
struct EditorCameraInput {
    float MouseDX, MouseDY;     // pixels this frame
    float Wheel;                // notches this frame
    bool  Look;                 // RMB held
    bool  Pan;                  // MMB held
    bool  Orbit;                // Alt + LMB held
    bool  W, A, S, D, Q, E;     // movement keys (held)
    bool  Frame;                // F pressed this frame
    bool  HasPivot;             // a selected entity exists
    glm::vec3 PivotCenter;      // selection AABB center (for orbit / frame)
    float     PivotRadius;      // selection bounding radius (for frame)
    float DeltaSeconds;
};
void  Update(const EditorCameraInput& in);
CameraView ToCameraView(float aspect) const; // View = (Ry*Rx)*translate(-Position); perspectiveRH_ZO(Fov, aspect, 0.1, 1000)
```

Behavior (Unity-style):
- **Look** (RMB held): `MouseDX/DY` → yaw/pitch (sensitivity const); pitch clamped ±89°.
- **Fly** (RMB held): WASD along view forward/right; Q/E down/up (world); `Wheel` adjusts `FlySpeed`.
- **Pan** (MMB drag): translate along camera right/up by mouse delta (scaled by distance/const).
- **Dolly** (`Wheel`, RMB **not** held): move `Position` along view forward.
- **Orbit** (Alt+LMB drag): rotate `Position` around `PivotCenter` (selection center, else scene
  origin) by mouse delta, preserving distance.
- **Frame** (`F`): when `HasPivot`, place `Position` along the current forward so `PivotCenter` is
  centered at a fit distance derived from `PivotRadius` and `Fov`; no-op otherwise.

The math is pure (no ImGui/NVRHI) → unit-testable.

### 4. Edit/Play toggle + overlay integration (`ImGuiRenderer`)

- State: `bool m_EditMode = true;` (default Edit on launch) and an `EditorCamera m_EditorCamera;`.
- **Toggle UI:** a button in the main menu bar / toolbar ("Edit"/"Play"), plus hotkey `F5`
  (`ImGui::IsKeyPressed(ImGuiKey_F5)` gated on `!io.WantTextInput`).
- **Per frame** in `ImGuiRenderer::Render` (after the viewport hover/focus + size are known):
  - `aspect = m_LastViewportW / m_LastViewportH`.
  - If `m_EditMode && m_ViewportHovered/Focused`: build `EditorCameraInput` from ImGui IO
    (`io.MouseDelta`, `io.MouseWheel`, mouse-button + key states, Alt mod), fill pivot from
    `SelectedEntity` (its transform/AABB via `MeshSys->GetMeshBounds` + `ModelMatrix`), call
    `m_EditorCamera.Update(in)`.
  - Publish: `App->EditorCameraActive.store(m_EditMode)`;
    `App->EditorCamera.store(m_EditorCamera.ToCameraView(aspect))`.
  - **Input routing:** in **Edit** mode the editor consumes viewport input → set
    `GameAcceptsMouse/Keyboard = false` (so the game's free-look doesn't also move); in **Play** mode
    keep today's behavior (route to game on viewport hover/focus). `F5` is read from ImGui IO on the
    RenderThread regardless of routing, so the toggle always works.
  - Store the editor camera matrices + `m_EditMode` into `EditorContext` for picking (section 5).

### 5. Picking uses the editor camera in Edit mode

Extend `EditorContext` with `bool EditorCameraActive; glm::mat4 EditorCamView, EditorCamProj;`
(requires including glm in the header). `PickEntity` chooses the camera:

```cpp
glm::mat4 V, P;
if (ctx.EditorCameraActive) { V = ctx.EditorCamView; P = ctx.EditorCamProj; }
else { const auto* cam = ctx.World->GetSingleton<WorldCameraComponent>();
       if (!cam) return INVALID_ENTITY; V = cam->View; P = cam->Projection; }
```

Entity iteration/ray-AABB is unchanged. This keeps the ray consistent with what the resolved
render camera drew (modulo the same 1-frame lag the render path already has).

## Data flow

```
Edit mode:
  ImGui IO (RenderThread) -> EditorCamera.Update -> ToCameraView
     -> ApplicationContext.EditorCamera (Seqlock) + EditorCameraActive=true
  next frame: Renderer.GetActiveCamera() = editor camera -> Mesh/Primitive/Outline passes
              EditorContext.EditorCam* -> PickEntity
  GameThread still ticks; its WorldCameraComponent is computed but ignored for the view.

Play mode:
  EditorCameraActive=false -> Renderer.GetActiveCamera() = snapshot WorldCameraComponent
  viewport input routed to the game (today's behavior); picking uses WorldCameraComponent.
```

## Runtime impact

Effectively none, by design. `runtime.exe` has no overlay, never writes the channel →
`EditorCameraActive` stays false → `Renderer::GetActiveCamera()` returns the snapshot
`WorldCameraComponent` (or identity), identical to today. The only shared-code change is the
3-pass camera read + the `ApplicationContext` fields (a few unused bytes in runtime). Edit/Play,
fly controls, picking-camera selection, and the toggle UI are all in `src/editor`, not linked by
runtime.

## Build / verification

Build preset `msvc-win64-vs2026-community`. `ApplicationContext.h` is shared → rebuild
engine/editor/runtime (+game). No `GAME_API_VERSION` bump.

- **Unit test (`test_editorcam`)** — new target alongside `test_picking`/`test_frustum`. Pure-math
  assertions on `EditorCamera`:
  - `View` of a camera at a known pose maps a known world point to the expected view-space point.
  - Yaw/pitch rotate the forward axis as expected; pitch clamps at ±89°.
  - Forward/right movement (WASD) translates `Position` along the correct basis vectors.
  - Orbit preserves distance to the pivot.
  - `Frame` places the pivot centered and fully within the frustum (project center → near NDC origin;
    bounding radius fits). Prints `All editor camera tests passed.`
- `editor` + `runtime` build clean; `test_ecs`/`test_alloc`/`test_frustum`/`test_input`/
  `test_picking` stay green.
- **GUI smoke (user-run):**
  - Edit mode (default): RMB-drag looks + WASD flies; wheel (RMB held) changes speed; MMB pans;
    wheel (no RMB) dollies; Alt+LMB orbits the selected entity; `F` frames the selection.
  - Picking + outline + gizmo operate through the editor camera in Edit mode.
  - Toggle to Play (button / `F5`): view snaps to the game camera; viewport input drives the game
    again; toggle back returns to the editor camera at its last pose.
  - `runtime.exe`: scene renders via the game camera exactly as before (no regression).

## Risks

- **Pass refactor regresses the no-override path** (runtime + Play mode) → mitigated by the
  byte-identical fallback invariant (§2) and the runtime smoke.
- **Seqlock misuse** — `CameraView` must stay trivially copyable; the `static_assert` enforces it at
  compile time.
- **Input double-drive** (editor cam + game free-look both moving) → mitigated by setting
  `GameAccepts*` = false in Edit mode.
- **Pick/render camera mismatch** — both read the editor camera in Edit mode; residual mismatch is
  only the existing 1-frame lag, acceptable as for selection/outline today.
- **Orbit/frame with no selection** → defined no-pivot fallbacks (scene origin; `F` is a no-op).
