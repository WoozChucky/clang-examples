# Login UI flow — design

**Date:** 2026-06-06
**Branch:** `feat/login-ui`
**Status:** DESIGN

## Goal

Gate the dual-server connection flow behind UI. Today the editor boots and `ClientSessionSystem` auto-connects (hardcoded `"player"`/`"stub"`) straight through to in-game. Instead: a **Start Game** button opens a **login screen** with real text entry (username/password); submitting drives a **Connecting** screen while the existing auth→world→char flow runs; reaching in-game enters the level. A failed attempt returns to the login screen with an error.

**Pure `Game.dll` work** — this consumes the just-merged engine/game boundary (game-defined components/singletons, game-owned `GameStateId`, raw input incl text on `GameState.FrameInputEvents`). **No engine, `ecs.dll`, or common edit.** The world/char selection screens, real authentication, a caret, and live per-step status are explicitly out of scope (auto-advance + stubbed auth stay).

## Context (as-built, verified)

- **States:** `src/game/include/GameStates.h` — `GameStateId { Uninitialized=0, MainMenu=1, InLevel=2, InEditor=3, Paused=4 }` + `StateIndex`/`AsGameState`. The engine treats the current state as an opaque `uint32_t` bit index (boundary Piece 2).
- **Menu seeding:** `game.cpp:900-921` (the no-`world.json` fallback boot block, gated by `g_GameState->StateId == GameStateId::Uninitialized`) creates a title + Play/Quit buttons. A button entity = `TransformComponent` (screen-pixel pos) + `UIRectComponent` (size/color) + `TextComponent` (label) + `MenuButtonComponent{ActionId}` + `StateScopeComponent{StateMask}`. `StateScope` masks are `1u << StateIndex(state)`.
- **`MenuInteractionSystem`** (`game.cpp:345-420`, Simulation): hit-tests scoped `MenuButton` entities vs UI-space mouse (file-local `ToUiSpace`/`PointInRect` helpers), drives `UIRectComponent.Color` (Normal/Hover/Press), arms on press / fires on release-inside → pushes an `ActionEvent` to `ActionQueueComponent`. Reads `InputStateComponent` (mouse pos + `MousePressed`/`MouseDown[MOUSE_BUTTON_LEFT]`).
- **`AppFlowSystem`** (`game.cpp:422-450`, Simulation, after MenuInteraction): consumes Nav-category actions → `SetState` via `ModifySingleton<GameStateComponent>(g.Current = StateIndex(s))`. Today: `Play→InLevel`, `Back→MainMenu`, `Quit→AppControl.QuitRequested`.
- **Actions:** `src/common/include/Actions.h` — `MakeAction(ActionCategory, uint16_t)`, `CategoryOf`, `namespace Actions { Play=Nav,1; Quit=Nav,2; Back=Nav,3 }`. `AppFlowSystem` only processes `CategoryOf == Nav`.
- **`ClientSessionSystem`** (`game.cpp:636-780`, PreRender): **auto-starts unconditionally** from `SessionState::Disconnected` (bounded retry: 20×0.5s then 5s) → `CreateClient` to `flow::kAuthPort`. Drives the `SessionFlow` FSM (`SessionFlow.h`, states Disconnected…InGame). `SendLogin` uses hardcoded `r.set_username("player"); r.set_password("stub")`. On `InGame` it flips `GameStateComponent→InLevel` (`~game.cpp:747`). Failures (`LoginFail`/`Dropped`) route the FSM back to `Disconnected` (retry).
- **Raw input:** `GameState.FrameInputEvents` / `FrameInputEventCount` (Piece 3), read via the `g_GameState` global. `InputEvent` (`input.h`): `TextEvent.Key` = Unicode codepoint (type `TextInput`); `KeyEvent` for keys. `InputStateComponent.Pressed[KEY_*]` for this-tick key presses (`KEY_BACKSPACE`, `KEY_ENTER`, `KEY_TAB`).
- **Text:** `TextComponent { std::string Text; glm::vec4 Color; size_t FontSize; }` — mutable each tick via `ctx.world.Modify<TextComponent>(e, fn)` (game systems run on the master ECS on GameThread).
- **Registration:** `GameRegisterSystems` (`game.cpp:782-799`); Simulation-phase order is registration order (MenuInteraction → AppFlow).
- **No login code exists** (`grep Login src/game` → only the wire `LoginReq`/`LoginResp` + `SessionAction::SendLogin`).

## Components

### New game-owned state values (`GameStates.h`)
Append `Login = 5` and `Connecting = 6`. Game-only change; the engine compares the opaque index, so no engine edit. `StateScope` masks use `1u << 5` / `1u << 6`.

### New game action (game-local, no common edit)
In `game.cpp`, define `constexpr uint32_t kSubmitLogin = MakeAction(ActionCategory::Nav, 20);` (Nav category so `AppFlowSystem`'s `CategoryOf==Nav` filter passes; `MakeAction`/`ActionCategory` are read-only from `Actions.h`). **Start Game** reuses `Actions::Play`; **Back** reuses `Actions::Back`.

### `LoginForm` — game singleton (Piece 1; runtime-only, not persisted)
Declared game-side (in `game.cpp` or a small game header), set via `SetSingleton<LoginForm>` — works with no `ecs.dll` edit thanks to header-instantiable templates.
```cpp
enum class LoginField : uint8_t { None, Username, Password };
struct LoginForm {
    std::string Username;
    std::string Password;
    std::string Error;                     // shown on the login screen after a failed attempt
    LoginField  Focused = LoginField::None;
    EntityId UsernameField = 0;            // entity ids of the display widgets, set at seed time
    EntityId PasswordField = 0;
    EntityId ErrorText     = 0;
};
```

### UI entities (seeded once at boot, `StateScope`-gated)
Added to the same boot block that seeds the menu, so they persist across states and show/hide by scope:
- **Login scope (`1u << 5`):** a "Login" title; a Username field (`UIRect` box + `TextComponent`); a Password field (`UIRect` + `TextComponent`); a **Submit** `MenuButton{kSubmitLogin}`; a **Back** `MenuButton{Actions::Back}`; an error `TextComponent` (red). The Username/Password field + error entity ids are stored into `LoginForm`.
- **Connecting scope (`1u << 6`):** a static "Connecting..." `TextComponent`.
- **MainMenu:** the existing "Play" button relabeled **Start Game** (still `Actions::Play`).

## Systems

### `LoginUISystem` (new — Simulation phase, registered after `MenuInteractionSystem`, before `AppFlowSystem`)
Active only when `GameStateComponent.Current == StateIndex(GameStateId::Login)`; otherwise returns early.
- **Focus:** on `InputStateComponent.MousePressed[MOUSE_BUTTON_LEFT]`, hit-test the UI-space mouse against the Username/Password field rects (reuse `ToUiSpace`/`PointInRect`) → set `LoginForm.Focused`. `Pressed[KEY_TAB]` cycles Username↔Password.
- **Typing:** iterate `g_GameState->FrameInputEvents`; for `TextInput` events with a printable codepoint (`>= 32`), append `(char)codepoint` to the focused field's string. `Pressed[KEY_BACKSPACE]` → `pop_back` the focused field (if non-empty). `Pressed[KEY_ENTER]` → push `kSubmitLogin` to `ActionQueueComponent` (same as clicking Submit). Typing clears `LoginForm.Error`.
- **Display:** each tick `Modify` the Username field `TextComponent.Text = Username` (or a dim placeholder when empty + unfocused), the Password field `Text = std::string(Password.size(), '*')`, and the error entity `Text = Error`. Highlight the focused field by setting its `UIRectComponent.Color` brighter.
- **Submit validation:** when the submit fires (button or Enter), if `Username` is empty set `LoginForm.Error = "Username required"` and do NOT push `kSubmitLogin` (gives a real error path even though the stub server accepts anything).

### `AppFlowSystem` (existing — edits in `game.cpp`)
- `Actions::Play` → `SetState(Login)` (was `InLevel`).
- `kSubmitLogin` → `SetState(Connecting)`.
- `Actions::Back` → `SetState(MainMenu)` (unchanged; from Login it returns to the menu).

### `ClientSessionSystem` (existing — gating edits in `game.cpp`)
- **Gate the start:** begin connecting only when `GameStateComponent.Current == StateIndex(GameStateId::Connecting)` AND `m_State == Disconnected`. Remove the unconditional from-`Disconnected` auto-start (the bounded-retry timer is no longer the trigger; the user submitting is).
- **Creds:** `SendLogin` reads `LoginForm` (`GetSingleton<LoginForm>`) — `set_username(Username)`, `set_password(Password)`.
- **Success:** on `InGame`, `SetState(InLevel)` (already present).
- **Failure:** when the FSM resets to `Disconnected` due to `LoginFail`/`Dropped`/etc. while connecting, set `LoginForm.Error` (e.g. "Login failed") and `SetState(Login)`. No auto-retry — the user re-submits (which returns to `Connecting`, re-triggering the gated start).

## Data flow (one tick, Login state)

`DrainInput` (engine) → `MenuInteractionSystem` (Submit/Back hover+click → action) + `LoginUISystem` (focus/typing/display; Enter → `kSubmitLogin`) → `AppFlowSystem` (consume `Play`/`kSubmitLogin`/`Back` → state change) → … → `ClientSessionSystem` (PreRender; if state is now `Connecting`, starts connecting with `LoginForm` creds). 1-tick responsive.

## Testing

- **Unit (pure):** extract `ApplyTextEdit(std::string& field, const InputEvent& ev)` (append printable `TextInput` codepoint; ignore non-printable) + a backspace helper, into a small game-side header; unit-test append/backspace/printable-filtering with synthetic `InputEvent`s (mirrors the `SessionFlow`/`test_authflow` style). This isolates the only non-trivial pure logic.
- **Manual smoke (human-owned):** run the editor (or `runtime.exe`): MainMenu shows **Start Game** → click → Login screen; click a field (highlights), type a username + password (password masks as `*`), Tab switches fields; Submit (or Enter) → "Connecting..." → enters the level (`GameState→InLevel`). Empty username + Submit → "Username required" error, stays on Login. (Failure-from-server path is hard to trigger since the stub auth accepts any non-empty creds; the client-side empty check exercises the error UI.)

## Out of scope (deliberate)

- World/character **selection screens** — the dual-server flow keeps auto-advancing world[0]/char[0].
- Real authentication / credential validation / persistence (auth stub accepts any non-empty creds).
- Blinking caret; live per-step Connecting status (static "Connecting..." only).
- Editing `LoginForm` in the editor inspector (it's runtime-only; not registered with the serializer registry).

## Decisions locked

- Login only; states `Login`/`Connecting` added game-side; `LoginForm` game singleton (runtime-only).
- Real text entry, focus-highlight only (no caret); password masked.
- Static "Connecting..." screen; failure → back to Login with error; no auto-retry.
- Start Game / Submit / Back are `MenuButton`s reusing `MenuInteractionSystem`; `kSubmitLogin` is a game-local Nav action; `AppFlowSystem` owns transitions; `LoginUISystem` owns the text fields; `ClientSessionSystem` gated on `Connecting` + reads `LoginForm` creds.
- Pure `Game.dll`: no engine/`ecs.dll`/common edit. No `GAME_API_VERSION` bump (no `GameState` layout change; `LoginForm` is an ECS singleton, not a `GameState` field).

## Prerequisite (RESOLVED — merged to main)

`LoginForm` is the first game-defined ECS component used in the live world. Game-defined component arrays were not hot-reload-safe (their `ComponentArray<T>` vtable/code lives in `Game.dll`; after `FreeLibrary` a virtual call/destructor on a surviving copy — the editor Memory panel's `MemoryBytes`, or a recycling RenderThread snapshot — hits dead code → crash). Fixed first as its own effort — **`game-component hot-reload safety`** (`docs/superpowers/specs|plans/2026-06-06-game-component-reload-safety*`), **merged to `main`**: adds `BuiltinComponentTypes()` + `ECS::RemoveNonBuiltinComponentArrays()` and a GameThread↔RenderThread reload barrier that clears game arrays + republishes a built-in-only snapshot before `FreeLibrary`. So `LoginForm` survives `Game.dll` reload; this login work is genuinely pure-`Game.dll` and reload-safe, and is the first real consumer exercising the game-component-survives-reload path end to end.

## Build / test note

Build & test with the `msvc-win64-vs2026-community` preset only. New game-defined component (`LoginForm`) + states + systems are `.cpp`-only game changes → hot-reload works; no `ecs.dll`/editor restart needed for iteration (the boundary work's payoff). Commit identity: `Nuno Silva <nuno.levezinho@live.com.pt>`.
