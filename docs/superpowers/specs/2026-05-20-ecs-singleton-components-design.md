# ECS Singleton Components + Input/Camera Systems Design

**Date:** 2026-05-20
**Status:** Draft — pending user review
**Scope:** Add a singleton-component mechanism to the ECS and use it to move global state (input, cameras, day/night config, app-control/quit) out of the `game.dll`-owned `GameState` struct and into the ECS, so the remaining per-tick `GameUpdate` logic (input drain, camera movement, free-look, quit) becomes engine drain + Simulation-phase systems. Follow-up to the ECS Systems layer (`docs/superpowers/specs/2026-05-20-ecs-systems-design.md`).

## Motivation

The Systems layer ships with a deliberately minimal `SystemContext { ECS& world; double dt; double gameTime; }`. Systems can only reach global state (input, cameras, settings) through the ECS — but that state currently lives in `GameState`. This feature defines *how* singleton/global state lives in the ECS, then migrates input + cameras + day/night config onto it, turning the last ad-hoc per-tick code in `GameUpdate` into systems. It also establishes the camera as a swappable system (FPS free-look now; top-down/iso later) behind a stable render interface.

## Decisions (from brainstorming)

1. **Singleton storage = components on a reserved ECS entity + typed sugar** (`SetSingleton`/`GetSingleton`/`ModifySingleton`). Full reuse of the existing component arrays / COW / snapshot machinery — no parallel storage path.
2. **Camera = resolved data + control behavior, split.** A render-facing `WorldCameraComponent` (view+projection matrices + position) is the stable interface; per-style control systems (`FreeLookCameraSystem` now, `IsoCameraSystem` later) write it. Swapping camera style = swapping the control system; render is unaffected.
3. **Two cameras, two singleton types:** `WorldCameraComponent` (3D, control-driven) and `UICameraComponent` (screen-space ortho, engine-maintained on resize).
4. **Render reads the camera from the ECS world snapshot** (not the Seqlock `SimulationSnapshot`), so camera + scene come from the same tick (temporal consistency); `SimulationSnapshot` loses its camera fields.
5. **Input drain is engine-side.** GameThread drains the `PlatformInput` ring into a raw `InputStateComponent` singleton before `scheduler.Run`. The platform ring never enters the ECS. Systems read the raw input singleton.
6. **No keybinds in the engine.** The engine emits raw input only. Quit is a game decision: a `QuitRequestSystem` (game) reads input and sets an `AppControlComponent` singleton the engine observes. Keybinds are configurable system state (e.g. `QuitRequestSystem`'s quit key is a constructor member, not hardcoded).
7. **`Clear()` preserves singletons** (session/engine state survives world loads + hot-reload).

## Architecture & data flow

```
ecs.dll
  Reserved singleton entity owned by ECS (created in ctor, recreated empty by Clear but
  KEPT through Clear, id preserved across CreateSnapshot). Singletons = components on it.
  Sugar:  SetSingleton<T>(v) / GetSingleton<T>() const / ModifySingleton<T>(fn)
  Hidden from GetActiveEntities()/GetEntityCount() → gameplay View<>(), the ImGui entity
  list, and WorldManager save/load never see it.

  New singleton component types (registered in the X-macro):
    InputStateComponent      { bool KeysDown[KEY_LAST+1]; bool Pressed[KEY_LAST+1];
                               double MouseX,MouseY,MouseDX,MouseDY; int32_t Wheel; }
    WorldCameraComponent     { glm::mat4 View; glm::mat4 Projection; glm::vec3 Position; }
    UICameraComponent        { glm::mat4 View; glm::mat4 Projection; }
    FreeLookControlComponent { float Yaw,Pitch,Fov,MoveSpeed,Sensitivity; bool MouseAimEnabled; }
    DayNightConfigComponent  { float CycleSeconds; }
    AppControlComponent      { bool QuitRequested; }
    ViewportComponent        { uint32_t Width, Height; }

GameThread tick (engine):
  drains (ECS cmds / model loads / render responses)
  → DRAIN INPUT: PlatformInput ring → ensure+write InputStateComponent singleton (raw only)
  → m_GameLib.Update(&gameState)             // init / state-machine / spawn (no per-tick input/camera)
  → m_Scheduler.Run({world, dt, gameTime})
        Simulation:      FreeLookCameraSystem (input+control → WorldCameraComponent)
                         TextRotationSystem
                         DayNightSystem (reads DayNightConfigComponent)
                         DebugSpawnSystem (F12)
        PostSimulation:  QuitRequestSystem (input → AppControlComponent.QuitRequested)
  → if GetSingleton<AppControlComponent>()->QuitRequested → stop
  → on resize: engine updates ViewportComponent + recomputes UICameraComponent
  → PublishSnapshot: CreateSnapshot() (carries singletons); SimulationSnapshot has no camera

RenderThread:
  worldSnapshot->GetSingleton<WorldCameraComponent>()  → 3D passes (Mesh, Primitive)
  worldSnapshot->GetSingleton<UICameraComponent>()     → UI pass / gizmo
  camera + scene from the SAME snapshot (temporally consistent)
```

**Threading unchanged:** GameThread is the single writer (drain + systems both run there); RenderThread reads the const snapshot. `CreateSnapshot` copies the singleton entity + components, so `GetSingleton` works on the const snapshot.

## Singleton mechanism (`ecs.dll`)

```cpp
class ECS {
    EntityId m_SingletonEntity;   // created in ctor; preserved through Clear(); copied by CreateSnapshot
public:
    template<typename T> void SetSingleton(T v)            { AddComponent(m_SingletonEntity, std::move(v)); }
    template<typename T> const T* GetSingleton() const     { return GetComponent<T>(m_SingletonEntity); }
    template<typename T, typename F> void ModifySingleton(F&& fn) { Modify<T>(m_SingletonEntity, std::forward<F>(fn)); }
};
```
- Thin wrappers over existing `AddComponent`/`GetComponent`/`Modify`; full COW + snapshot reuse, no new storage.
- `GetActiveEntities()`/`GetEntityCount()` exclude `m_SingletonEntity`.
- `CreateSnapshot()` preserves `m_SingletonEntity` (member + component arrays) so the render-side const `GetSingleton<T>()` works.
- `Clear()` removes gameplay entities but keeps `m_SingletonEntity` and its singleton components (so input/camera survive world loads).
- The seven new component structs are defined in `ECS.h` alongside the existing component catalog (`ECS.h` includes `Input.h` for `KEY_LAST`) and added to `ECS_FOR_EACH_REGISTERED_COMPONENT` so the explicit instantiations in `ecs.cpp` cover them.
- These singleton components need **no** `ECSCommands` handling (not edited via the inspector) and **no** `WorldManager` serialization (runtime state; the hidden entity is skipped by the save loop).

## Input pipeline

**Engine drain (GameThread, before `scheduler.Run`)** — replaces `game.cpp`'s `DrainInput`, produces raw input only:
```
ensure InputStateComponent exists (create-if-absent)
clear Pressed[]; prevMouse = (MouseX, MouseY)
for each event in PlatformInput ring:
    Key PRESS/REPEAT → KeysDown[k]=true;  PRESS also → Pressed[k]=true
    Key RELEASE      → KeysDown[k]=false
    MouseMove        → MouseX,MouseY = latest cursor pos
    MouseWheel       → Wheel = offset
MouseDX = MouseX-prevMouseX;  MouseDY = MouseY-prevMouseY
write InputStateComponent
```
The engine applies **no** sensitivity / mouse-aim gating / first-mouse clamp / keybind meaning — all of that is system-side, so a different camera system can interpret the same raw input differently. ESC is **not** special-cased (quit is a game decision, below).

## Camera

**Components:** `WorldCameraComponent` (resolved, render-facing), `FreeLookControlComponent` (control state), `UICameraComponent` (engine-maintained), `ViewportComponent` (window dims for aspect).

**`FreeLookCameraSystem` (game, Simulation)** reproduces today's `HandleCameraMovement` + `HandleFreeLook`:
```
read InputStateComponent + FreeLookControlComponent + ViewportComponent
- T pressed   → toggle ctrl.MouseAimEnabled (recenter on enable)
- WASD/QE/Space/Shift + dt → move/rotate using ctrl.MoveSpeed; forward/right from Yaw/Pitch
- wheel       → adjust ctrl.Fov (clamp)
- mouse delta → if MouseAimEnabled, apply ctrl.Sensitivity to Yaw/Pitch (clamp pitch)
write WorldCameraComponent:
    Position   = updated position
    View       = from Yaw,Pitch,Position
    Projection = perspective(ctrl.Fov, ViewportComponent aspect, near, far)
```
**UI camera:** engine recomputes `UICameraComponent` (ortho from `ViewportComponent`) on resize — no control system.

**Camera-style swap (future, out of scope to implement):** register `IsoCameraSystem` instead of `FreeLookCameraSystem`; both write `WorldCameraComponent`. Render unchanged.

## Quit / app-control (game-owned)

- `QuitRequestSystem` (game, PostSimulation) holds its quit key as a constructor member:
  ```cpp
  explicit QuitRequestSystem(int quitKey = KEY_ESCAPE);
  void Update(ctx) { if (input->Pressed[m_QuitKey]) ModifySingleton<AppControlComponent>(a.QuitRequested=true); }
  ```
  Registered as `std::make_unique<QuitRequestSystem>(KEY_ESCAPE)`. The engine ships no keybind; the game owns the binding + the "when."
- GameThread, after `scheduler.Run`, reads `GetSingleton<AppControlComponent>()->QuitRequested` and stops — replacing the old `gameState.QuitRequested` check.
- The window close button remains a separate engine/OS path (PlatformThread → `ShutdownRequested`).

## Migration & cleanup

**`GameState` fields REMOVED** (bumps `GAME_API_VERSION` 4→5): `KeysDown[]`, `MouseAimEnabled`, `MouseX/Y` (→ `InputStateComponent`/`FreeLookControlComponent`); `GameCamera`, `UICamera` (→ camera components); `DayNightCycleSeconds` (→ `DayNightConfigComponent`); `QuitRequested` (→ `AppControlComponent`); `PlatformInput` ptr (engine drains `ApplicationContext::InputRing` directly).

**`GameState` KEEPS:** `StateId`, `GameTime`, `DeltaTime`, `TargetTPS`, `ActualTPS`, `Settings`, `GameOutputHandle`, `World`, `WorldLoaded`.

**`GameUpdate`:** `Uninitialized` seeds singletons (`FreeLookControlComponent` initial yaw/pitch/fov/pos = today's `(0,5,10)`; `WorldCameraComponent`; `AppControlComponent{}`; `DayNightConfigComponent{10.0f}`) then spawns the default scene if `!WorldLoaded`. `MainMenu` becomes `break;`. No per-tick input/camera code remains.

**Engine init (GameThread):** `SetSingleton<InputStateComponent>({})` + `SetSingleton<ViewportComponent>(window dims)` at startup; update `ViewportComponent` + recompute `UICameraComponent` on resize (`GameResize` path).

**`GameRegisterSystems`:** `FreeLookCameraSystem`, `TextRotationSystem`, `DayNightSystem` (now reads `DayNightConfigComponent`), `DebugSpawnSystem` (F12), `QuitRequestSystem(KEY_ESCAPE)`.

**Snapshot:** `SimulationSnapshot` loses `GameCamera`/`UICamera` (keeps `Tick`/`Timestamp`/TPS/`FrameStats`/`ObjectX`); `PublishSnapshot` drops the camera copy; the init snapshot in the GameThread ctor adjusts.

**Render-pass edits** (passes already receive `const ECS* world`): `MeshRenderPass`/`PrimitiveRenderPass` → `world->GetSingleton<WorldCameraComponent>()->View/Projection`; `UiRenderPass` → `world->GetSingleton<UICameraComponent>()`. `ImGuiRenderer::Render(...)` gains the `const ECS* world` parameter (it currently gets only the `SimulationSnapshot`) so the gizmo can `GetSingleton` the camera. Replace every `snapshot.GameCamera.get_view_matrix()` / `get_projection_matrix()` call site.

## Error handling

| Case | Behavior |
|---|---|
| `GetSingleton<T>()` before seeded | Returns `nullptr`. Systems null-check (no-op that tick). Render null-checks the camera → identity fallback (degenerate, no crash) for the brief pre-seed window. |
| `ModifySingleton<T>` on absent component | No-op. Engine drain **ensure-creates** `InputStateComponent`, so input self-heals. |
| World load (`Clear`) | Singletons survive (Clear keeps the reserved entity) → input/camera uninterrupted. |
| Hot-reload | Singletons live in `GameState.World` → persist; `FreeLookControlComponent` yaw/pitch survive (no camera jump); systems re-registered by the Systems-layer machinery. |
| Reserved entity | Hidden from `GetActiveEntities`/count; not destroyable via gameplay paths. |

**Invariants:** the reserved singleton entity always exists after construction; `Clear()` never removes it; `CreateSnapshot` always carries it; the platform input ring never enters the ECS; the engine sets no keybinds.

## Testing

**`test_ecs.exe` (pure ecs.dll, unit-testable):**
- `SetSingleton`/`GetSingleton` round-trip; `GetSingleton` null when unset.
- `ModifySingleton` mutates; no-op when unset.
- Reserved singleton entity excluded from `GetActiveEntities()` + `GetEntityCount()`.
- `CreateSnapshot` preserves singletons (`GetSingleton` on the const snapshot returns the value).
- `Clear()` removes gameplay entities but keeps singletons.

**Manual smoke (systems need input events + GPU):** camera moves (WASD / mouse-aim toggle `T` / wheel zoom) via `FreeLookCameraSystem`; `ESC` quits via `QuitRequestSystem`→`AppControlComponent`; text spins + day/night cycle intact; `F12` spawns; window resize updates the UI camera + aspect; hot-reload re-registers systems with camera/input preserved (no camera jump); loading a world keeps input/camera alive.

## Build / rebuild

`ECS.h` gains the seven singleton component types + X-macro entries + `#include "Input.h"`; `ecs.cpp` instantiations cover them. `GAME_API_VERSION` 4→5 (GameState layout changed). Rebuild `ecs` + `editor` + `game` + `test_ecs`; restart the editor (ABI/layout change).

## Out of scope (follow-ups)

- The iso/top-down camera itself (the design proves it's "write one system"; not implemented here).
- A full key→action mapping / rebindable-keybinds layer (this feature provides raw input + per-system key config as the foundation).
- Moving `Settings`/timing into singletons; turning `GameUpdate`'s state machine + spawn into systems.
- Inspecting/editing singleton components via the ImGui inspector.
- `View<>()` allocation optimization.
- Parallel systems.
