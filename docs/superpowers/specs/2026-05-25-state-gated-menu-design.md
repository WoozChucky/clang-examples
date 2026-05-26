# State-Gated Game Logic + Data-Driven Main Menu — Design

**Date:** 2026-05-25
**Status:** Approved (design); pending spec review
**Scope:** Engine + ECS + editor + game. Multi-layer; delivered in phases (see §15).

## Problem

`GameStateId` exists but does nothing: systems run every tick regardless of state, and
scene/UI is driven entirely by `world.json` + always-on systems. We want (a) gameplay that
runs only in the right state, and (b) a basic main menu (labels + clickable buttons) that is
**authored in the editor like any other content** — not spawned by hardcoded game code — yet
only appears/works in the MainMenu state. The action a button triggers must scale to a large
game without becoming a 1000-case switch.

## Mental model: data vs behavior (the boundary)

The whole design follows one rule:

- **ECS / `world.json` = data (nouns):** what exists, where, color, text, *which action it
  fires*, *which state(s) it belongs to*. Authored + tweaked live in the editor, persisted.
- **`game.dll` = behavior (verbs):** the state machine, systems that act on data, and *what
  an action means*. Code.

Consequences that resolve the prototyping conflict:
- UI is **not** spawned by code. It is authored as entities and **gated by filtering its
  activity, not its existence** — entities always exist; a scope tag + the current state
  decide where they render and respond.
- A click is **not** a function call. It **emits an event/intent**; the system that owns that
  domain consumes it. Cross-talk happens through components, never direct calls. This keeps
  the action layer decoupled and scalable.

## Architecture overview (layers)

1. **State machine** — `GameStateComponent` singleton (in the ECS snapshot) is the single
   source of truth for the current state; systems read it to gate; the renderer reads it to
   filter; an `AppFlowSystem` owns transitions.
2. **State scoping** — `StateScopeComponent` tags an entity to one or more states; the UI
   renderer and the menu-interaction system only act on entities scoped to the current state.
3. **Input** — mouse buttons drained into `InputStateComponent`; viewport origin published so
   editor click coords map into UI space.
4. **UI primitives** — `UIRectComponent` (authorable solid quad) rendered via the UI pass's
   existing solid-color path; text + rects filtered by scope.
5. **Action layer** — `MenuButtonComponent` carries an `ActionId` (data); a click pushes an
   `ActionEvent` into an `ActionQueue` singleton; owning systems consume the actions in their
   category. `ActionId` is namespaced by category for scale.

---

## 1. State machine

- **Move `GameStateId`** from `src/game/include/game.h` into a common header
  (`src/common/include/GameStateId.h`, or inline in `ECS.h`); `game.h` includes it and drops
  its local copy. `GameState.StateId` keeps working unchanged.
- **`GameStateComponent { GameStateId Current = GameStateId::MainMenu; }`** — singleton, in
  `ECS.h` + X-macro. Lives in the ECS so it is part of the deep-copied snapshot the render
  thread reads. **Not persisted** in `world.json` (runtime state; seeded at boot).
- **Single source of truth:** `GameStateComponent.Current` is authoritative. The host
  `GameState.StateId` becomes a **mirror** — `GameUpdate` copies `Current → StateId` at the top
  of each tick so any host/editor code reading `StateId` stays correct, but no logic keys off
  `StateId` anymore. The `GameStateId::Uninitialized` value on the host field is used **only**
  as the one-time boot guard: on the first tick `GameUpdate` seeds all singletons (incl.
  `GameStateComponent{ Current = MainMenu }` + `ActionQueueComponent`), loads/falls-back the
  scene, and leaves `StateId` mirroring `Current`. After boot, **`AppFlowSystem` is the only
  writer of `Current`**; everything else reads it.
- **Gameplay gating:** `PlayerMovementSystem`, `CameraZoomSystem`,
  `IsometricFollowCameraSystem` early-out unless `Current == InLevel`:
  ```cpp
  const auto* gs = ctx.world.GetSingleton<GameStateComponent>();
  if (!gs || gs->Current != GameStateId::InLevel) return;
  ```
- **`AppFlowSystem`** (new, runs after the interaction system) consumes navigation actions
  from the `ActionQueue` and performs transitions (`Play → InLevel`, `Quit →
  AppControlComponent.QuitRequested`, `Back/ESC → MainMenu`). The state machine lives here,
  in one place, not scattered across the switch.
- **`GameUpdate` switch shrinks** to boot seeding only (Uninitialized: seed singletons incl.
  `GameStateComponent` + `ActionQueue`, load/fallback scene, → MainMenu). Per-state behavior
  moves into systems.

## 2. State scoping

- **`StateScopeComponent { uint32_t StateMask = 0; }`** — `ECS.h` + X-macro, authorable +
  persisted. A bit per state (`1u << uint32_t(GameStateId::X)`). `StateMask == 0` (or no
  component) = always-on (e.g. a persistent HUD).
- Helper `bool ScopeAllows(uint32_t mask, GameStateId cur)` = `mask == 0 || (mask & (1u <<
  uint32_t(cur)))` — pure, unit-tested.
- The UI renderer and menu interaction skip entities whose scope doesn't allow the current
  state. So an authored menu entity exists in every state but only renders/responds in its
  scoped state(s).

## 3. Input: mouse buttons + viewport origin

- **`InputStateComponent`** gains `bool MouseDown[8] = {}` and `bool MousePressed[8] = {}`
  (mirrors `KeysDown`/`Pressed`). `GameThread` drain handles `InputEventType::MouseButton`
  (PRESS → `Down`+`Pressed`, RELEASE → clear `Down`) and clears `MousePressed` each tick like
  keys/wheel.
- **`ViewportComponent`** gains `OriginX, OriginY` (top-left of the scene viewport in window
  coords). The editor already publishes the viewport *size* via `ApplicationContext`; it also
  publishes the origin; `GameThread` writes it into `ViewportComponent`. Runtime origin = 0.
- **UI-space mouse** = `(InputState.MouseX - Viewport.OriginX, MouseY - OriginY)`. This makes
  menu clicks correct **in the editor viewport too**, not only `runtime.exe`.

## 4. UI primitives (rendering)

- **`UIRectComponent { glm::vec2 Size; glm::vec4 Color; }`** — `ECS.h` + X-macro, authorable +
  persisted. A solid screen-space quad positioned by `TransformComponent` (pixels), reusable
  for panels/backgrounds.
- **`UiRenderPass`** emits a `UIRectComponent` entity as one instanced quad with the existing
  `SAMPLE_TEXTURE` flag **off** (solid `Color`), **before** the text glyph instances so labels
  draw on top. Both rects and text are filtered by `ScopeAllows(scope, current)` — the pass
  reads `GameStateComponent.Current` from the snapshot.
- Z/order within the UI pass: rects first, then glyphs (2D alpha blend, draw-order layering).

## 5. Action layer (scalable dispatch)

- **`ActionEvent { uint32_t ActionId; EntityId Source; uint64_t Param = 0; }`** — small POD.
- **`ActionQueueComponent { std::vector<ActionEvent> Events; }`** — singleton, `ECS.h` +
  X-macro, **not persisted**. Cleared at the start of each tick (like `InputState.Pressed`).
  Producers push; consumer systems drain. (The per-tick vector is tiny; snapshot copy is
  negligible.)
- **`ActionId` namespacing** in a common header (`src/common/include/Actions.h`):
  ```cpp
  enum class ActionCategory : uint16_t { None = 0, Nav = 1 /*, Inventory, Ability, ... */ };
  constexpr uint32_t MakeAction(ActionCategory c, uint16_t local) {
      return (uint32_t(c) << 16) | local;
  }
  namespace Actions {
      inline constexpr uint32_t None = 0;
      inline constexpr uint32_t Play = MakeAction(ActionCategory::Nav, 1);
      inline constexpr uint32_t Quit = MakeAction(ActionCategory::Nav, 2);
      // future categories add their own block; systems claim a category each.
  }
  constexpr ActionCategory CategoryOf(uint32_t id) { return ActionCategory(id >> 16); }
  ```
- **Ownership, not a god switch:** each system handles only the actions in *its* category. The
  prototype's `AppFlowSystem` owns `Nav`. Adding a domain later = add a system that owns its
  category, not a case in a central file. Per-system switches stay small.
- **Data flow patterns** (how a system interacts with / gets info from an action):
  - *Transient:* `ActionEvent.Param` carries data the consuming system reads that tick.
  - *Durable:* a handler writes a component the system queries (intent → state).
  - *Feedback (system → UI):* a system writes a component the UI reads (e.g. a future
    `Notification`); never a direct call back. Everything is messages through components.
- **Existing precedent:** `AppControlComponent.QuitRequested` already is this pattern (the
  quit "action" writes a component; the app loop reacts). This generalizes it.

## 6. Menu authoring (data)

- A menu button is **one authored entity**: `TransformComponent` (pos) + `UIRectComponent`
  (bg) + `TextComponent` (label) + `MenuButtonComponent` + `StateScopeComponent{ MainMenu }`.
- **`MenuButtonComponent { uint32_t ActionId = 0; glm::vec4 Normal; glm::vec4 Hover; glm::vec4
  Press; }`** — `ECS.h` + X-macro, authorable + persisted. `ActionId` is the data binding to
  behavior; the three colors drive visual states.
- **Editor inspector:** add/remove/edit for `UIRectComponent`, `StateScopeComponent`,
  `MenuButtonComponent` (same pattern as `PlayerComponent`). `MenuButtonComponent.ActionId`
  surfaces as a named dropdown from `Actions.h`; `StateScopeComponent` as state checkboxes.
- **Serialization:** `UIRectComponent`, `MenuButtonComponent`, `StateScopeComponent` round-trip
  per-entity in `world.json` (`ComponentSerialization.h` + the WorldManager save/load loops),
  exactly like `PlayerComponent`. (`TransformComponent`/`TextComponent` already persist.)

## 7. Menu interaction (behavior)

- **`MenuInteractionSystem`** (new), gated to states that have menus (reads
  `GameStateComponent.Current`). Each tick, for every entity with `MenuButtonComponent` +
  `UIRectComponent` + `TransformComponent` whose `StateScope` allows the current state:
  - Compute the rect in UI space; `PointInRect(uiMouse, pos, size)` → hovered.
  - Set `UIRectComponent.Color` = Press (mouse held inside) / Hover (inside) / Normal.
  - **Click latch:** mouse-pressed-inside then released-inside on the same button = a click →
    push `ActionEvent{ ActionId = btn.ActionId, Source = entity }` into `ActionQueue`.
- Runs **before** `AppFlowSystem`, which consumes the events the same tick.
- Hit-test + click-latch are **pure helpers** (`MenuHitTest.h`) → unit-tested.

## 8. Control flow per tick

1. `GameThread` drains input → `InputStateComponent` (keys, mouse move/wheel, **mouse
   buttons**), writes `ViewportComponent` (size + origin).
2. `GameUpdate`: clears `ActionQueue.Events`; boot seeding on first tick; otherwise thin.
3. Scheduler runs systems in order:
   - `MenuInteractionSystem` (scoped) → hover colors + emits `ActionEvent`s.
   - `AppFlowSystem` → consumes Nav actions → writes `GameStateComponent.Current` /
     `AppControlComponent.QuitRequested`.
   - Gameplay systems (`PlayerMovement`/`CameraZoom`/`IsometricFollowCamera`) → gated to
     `InLevel`, no-op in MainMenu.
4. `GameThread` publishes the snapshot.
5. Render thread reads the snapshot; `UiRenderPass` filters rects+text by
   `GameStateComponent.Current` + `StateScope`.

Net: gameplay frozen in MainMenu; menu visible + clickable only in MainMenu; Play → InLevel
resumes gameplay + hides menu; ESC in InLevel → MainMenu.

## 9. Prototype content

- `world.json` (or the code fallback) holds: a title `TextComponent` and **Play** + **Quit**
  buttons scoped to MainMenu, plus an optional **Settings** button bound to an action with no
  consumer yet (demonstrates "add the binding now, add the owning system later" — it's a
  harmless no-op until a system claims it).
- `Play → InLevel`, `Quit → QuitRequested`. In `InLevel`, ESC emits a `Back` nav action →
  `AppFlowSystem` → MainMenu (replaces the always-quit `QuitRequestSystem`, which is
  removed/repurposed).

## 10. Fallback (dev convenience)

When no `world.json` is present, the boot path code-spawns the same menu entities (mirroring
today's default-scene fallback) so the menu is usable without authoring. Authored data is the
primary path; the fallback is a convenience and uses the identical components.

## 11. Testing

- **Pure helpers, unit-tested** (`test_menu`, glm-only link, project pattern):
  - `ScopeAllows(mask, current)` — always-on, single-state, multi-state, mismatch.
  - `PointInRect(p, pos, size)` — inside/edge/outside.
  - Click latch (press-inside→release-inside = click; press-inside→release-outside = no
    click; release without press = no click).
  - `MakeAction`/`CategoryOf` round-trip + `Nav` routing.
- **Serialization round-trip** for the three new authorable components (extend
  `test_worldserial`).
- **Manual GUI smoke** (DX12 + Vulkan): author a button in the editor, scope it to MainMenu,
  see it only there; hover/press colors; Play→InLevel (gameplay resumes, menu hides); ESC
  back; Quit exits; clicks correct **in the editor viewport** (origin offset) and in
  `runtime.exe`.

## 12. Touch list (by layer)

- **Common:** `GameStateId.h` (moved enum), `Actions.h` (action ids/categories); `ECS.h`
  (`GameStateComponent`, `StateScopeComponent`, `UIRectComponent`, `MenuButtonComponent`,
  `ActionQueueComponent`, `InputStateComponent` mouse fields, `ViewportComponent` origin, + 5
  X-macro entries); `Systems.h` (unchanged — systems read singletons);
  `ComponentSerialization.h` (3 authorable components); `MenuHitTest.h` (pure helpers).
- **Engine:** `GameThread` (mouse-button drain, viewport origin); `UiRenderPass`
  (`UIRectComponent` solid-quad emission + scope filter on rects + text); `WorldManager`
  (save/load the 3 authorable components).
- **Editor:** publish viewport origin; `EcsInspectorPanel` (add/remove/edit + ActionId
  dropdown + scope checkboxes for the 3 authorable components).
- **Game (`game.cpp`):** `AppFlowSystem`, `MenuInteractionSystem`; gate gameplay systems to
  `InLevel`; shrink `GameUpdate` switch to boot seeding; remove/repurpose `QuitRequestSystem`;
  register new systems in order. `game.h` includes the moved `GameStateId`; bump
  `GAME_API_VERSION`.
- **Tests:** `test_menu` (+ `tests/CMakeLists.txt`); extend `test_worldserial`.

## 13. Editor caveat — resolved

Game mouse coords are window-space; the editor scene is a viewport sub-rect. The viewport
origin (§3) is published and subtracted so clicks land correctly in the editor viewport.
`runtime.exe` origin = 0 (unchanged).

## 14. Non-goals (YAGNI)

- No JSON-data-driven action *registry* yet (ActionIds are a compile-time enum; the editor
  dropdown reads it). Data-file registry is a later prod concern.
- No full settings menu, sliders, text input, scrolling, or nav-stack/focus model.
- No menu transitions/animations, no gamepad/keyboard focus navigation.
- No multi-font/theming system; reuse the existing font atlas.

## 15. Phased delivery

Each phase is independently buildable + testable; ship/merge per phase.

- **Phase 1 — State machine:** `GameStateId` → common; `GameStateComponent` singleton; gate
  the 3 gameplay systems to `InLevel`; `AppFlowSystem` skeleton + `ActionQueueComponent`
  (cleared per tick); shrink the `GameUpdate` switch. *Test:* gameplay runs only in InLevel;
  state transitions via a temporary keyboard trigger.
- **Phase 2 — Input + viewport origin:** mouse buttons in `InputStateComponent` + drain;
  `ViewportComponent` origin + editor publish + UI-space mouse mapping. *Test:* game reads
  clicks at correct coords in editor + runtime.
- **Phase 3 — Authorable UI primitives:** `UIRectComponent` + `StateScopeComponent` + UI-pass
  solid-quad emission + scope filter (rects + text) + serialization + inspector. *Test:*
  author a scoped rect in the editor; it renders only in its state; round-trips in world.json.
- **Phase 4 — Menu + actions:** `MenuButtonComponent`, `ActionEvent`, `MenuInteractionSystem`
  (hit-test → emit), `AppFlowSystem` Nav handling, Play/Quit/ESC, fallback menu, `Actions.h`.
  *Test:* full menu — hover/click, Play→InLevel, ESC→MainMenu, Quit.

Phases 1–2 are engine/ECS foundations; 3–4 deliver the visible menu. `GAME_API_VERSION` bumps
once per phase that changes `ECS.h`/`GameState`/exports.
