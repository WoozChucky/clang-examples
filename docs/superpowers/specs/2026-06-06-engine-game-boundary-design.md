# Engine/Game boundary: game-owned ECS extensibility, states, raw input — design

**Date:** 2026-06-06
**Branch:** `feat/engine-game-boundary`
**Status:** DESIGN

## Goal

Let `Game.dll` own its own gameplay state without forcing engine/`ecs.dll` rebuilds or editor restarts. Today adding a single ECS component requires editing `ECS.h`, `ECSCommands.h`, and `ComponentSerialization.h`, rebuilding `ecs.dll` + editor + game, and restarting the editor — game-specific needs leak into the engine/common layer and the workflow does not scale.

Close three verified boundary leaks and add the extensibility they imply:

1. **Game-defined components/singletons** instantiated entirely in `Game.dll` (no `ecs.dll` rebuild, no restart) — stored, snapshotted, COW-cloned like built-ins.
2. **Game-owned game states** — the `GameStateId` enum moves to a game header; the engine treats the current state as an opaque `uint32_t` bit index.
3. **Raw input (including text) surfaced to the game** — the engine exposes this tick's raw `InputEvent`s to `GameUpdate`; the game decides what they mean.

Plus the two extensibility features that ride on (1):

4. **Registered serialization** — game components round-trip through `world.json` via game-provided (de)serializers; the engine's save/load loop becomes generic (removes the explicit per-type lists).
5. **Editor inspector editing of game components** — the editor inspects/edits any registered component generically (JSON round-trip through the registered (de)serializers), with no ImGui dependency in `Game.dll`; the `ECSCommands` per-type dispatch becomes generic too.

The login-UI flow (separate downstream spec, parameters captured in the appendix) is the first real consumer and must end up **pure `Game.dll`**: no engine touch, no restart.

## Context (as-built, verified)

- **Component storage is already runtime type-erased.** `ECS` holds `std::unordered_map<std::type_index, std::shared_ptr<IComponentArray>> m_ComponentArrays` (`src/common/include/ECS.h:695`). `GetComponent<T>`/`AddComponent<T>` resolve `T` to a slot via `std::type_index(typeid(T))`.
- **The gate is compile-time instantiation, not storage.** Template method *bodies* live in `src/ecs/src/ecs.cpp` (`ComponentStore::MutateArray/GetArray/AddComponent/...` at lines 85–145; `ECS::AddComponent/...` from line 149) and are emitted only for the X-macro set via explicit instantiation (`ecs.cpp:79, 137–145, 282–292`). `ECS.h:586–590` declares `extern template class ECS_API ComponentArray<T>` for every registered `T`, so consumers (editor, game, tests) link the single copy in `ecs.dll`. A type **not** in the X-macro has no body visible to consumers → unresolved external at link.
- **COW clone + pool are file-local to `ecs.cpp`.** `ComponentArrayPool<T>`, `GetArrayPool<T>()`, `MakePooledClone<T>()`, and `ArrayPoolCounters()` are in an anonymous namespace (`ecs.cpp:6–68`); `ComponentArray<T>::Clone()` is out-of-line (`ecs.cpp:72–75`) specifically so it can reach that file-local pool. `IComponentArray` already exposes a virtual `Clone() -> std::shared_ptr<IComponentArray>`.
- **Snapshot is generic.** `ECS::CreateSnapshot()` (`ecs.cpp:243–255`) shallow-copies the whole array map (shared_ptr refcount bump); the COW clone fires on the next `MutateArray<T>` on the master (`ecs.cpp:86–98`). It does **not** iterate the X-macro — a game component already rides the snapshot if its array exists in the map.
- **Serialization is explicit and engine-side.** `WorldManager.cpp` save (`:14–107`) and load (`:109–183`) hardcode `HasComponent<T>`/`contains("T")` for each built-in type + singleton. Per-type (de)serializers are inline in `src/common/include/ComponentSerialization.h`.
- **ECSCommands is editor-edit-only.** `ECSCommandProcessor::ApplyComponentCommand`/`RemoveComponentByType` (`ECSCommands.h:269–401`) have explicit per-type branches; fed only by the editor's ImGui inspector via the `ECSCommandRing` (RenderThread→GameThread). Core sim does not use it.
- **`GameStateId` is purely game-owned in practice.** Defined in `src/common/include/GameStateId.h`. The engine's only reference is **read-only** in `UiRenderPass.cpp`: it reads `GameStateComponent.Current` and uses it as a bit index for `StateScopeComponent.StateMask`. The engine never writes it and never branches on specific values. `game.cpp` is the exclusive writer.
- **Raw input never reaches the game.** `GameThread::DrainInputToSingleton` (`GameThread.cpp:615–654`) pops the entire `ApplicationContext::InputRing` (SPSC, 256) into `InputStateComponent` and handles only Key/MouseMove/MouseWheel/MouseButton — `InputEventType::TextInput` (`input.h:166`) is silently dropped. `GameState` (`src/game/include/game.h:28–39`) exposes no input ring; the game only sees the digested `InputStateComponent` singleton.

## Pieces

The five pieces are independently landable and testable; recommended order is 1 → 2 → 3 → 4 → 5 (4 and 5 share the registry; 5 depends on 4).

### Piece 1 — Header-instantiable ECS templates (core)

**Change:** move the template machinery from `ecs.cpp` into `ECS.h` so any module can instantiate `ComponentArray<T>` and the `ComponentStore`/`ECS` `<T>` methods for its own `T`:

- The `ComponentStore` and `ECS` templated method bodies (`MutateArray`, `GetArray`, `AddComponent`, `RemoveComponent`, `HasComponent`, `GetComponent`, and the `ECS::` equivalents).
- The COW/pool machinery: `ComponentArrayPool<T>`, `GetArrayPool<T>()`, `MakePooledClone<T>()`, and `ComponentArray<T>::Clone()`.

**Keep for built-ins:** the explicit `template class ComponentArray<T>` + method instantiations and the `extern template` decls remain in `ecs.cpp`/`ECS.h` **driven by the X-macro**. Built-ins still live as one exported copy in `ecs.dll` (no code bloat, single RTTI identity). A game type is simply absent from the X-macro, so including `ECS.h` and using it instantiates the machinery **locally in `Game.dll`**.

**RTTI safety:** a game type is named (`typeid`) only inside `Game.dll`; the engine never names it (it touches game arrays only through `IComponentArray` virtuals). So `std::type_index` identity for game types is single-module and safe. Built-ins keep their single `ecs.dll` identity. No cross-DLL `type_info` equality is required.

**Pool counters observability:** `ComponentArrayPool<T>`/`ArrayPoolCounters()` are currently file-local, so moving them to the header gives each module its own counter instance — the editor Memory panel (reads `ecs.dll`'s) would miss game-module pools. Fix: export a single `ArrayPoolCounters()` accessor from `ecs.dll` and have the header pool reference it, so all modules' pool stats aggregate into one place. (Built-in pools are unaffected.)

**Result:** `world.AddComponent<GameFoo>(e, {...})`, `world.SetSingleton<GameFoo>({...})`, `GetComponent<GameFoo>`, removal, snapshot, and COW clone all work from `Game.dll` with zero `ecs.dll` edit and no restart. Game singletons survive hot-reload because they live in the host-owned `GameState.World`, not DLL statics.

**Risk / must-verify:** the pool path when instantiated outside `ecs.dll`. The test (below) defines a component type **in the test translation unit** (outside `ecs.dll`) and exercises add/get/remove + snapshot + COW to prove header instantiation and the pool deleter work cross-module.

### Piece 2 — Game-owned game states

- `GameStateComponent.Current` changes type from `GameStateId` to `uint32_t` (the bit index the engine already treats it as).
- Move the `GameStateId` enum from `src/common/include/GameStateId.h` into a game-owned header under `src/game/`. The game casts to/from `uint32_t` at the `GameStateComponent` boundary.
- `UiRenderPass.cpp` stops including the game enum; it reads `GameStateComponent.Current` as `uint32_t` and compares the bit index against `StateScopeComponent.StateMask` exactly as today.
- The game may add/rename/reorder its states freely; only the bit indices (0–31) are an implicit contract with `StateScopeComponent` masks, which the game also owns.

`GameStateComponent` itself stays a built-in registered component (the renderer reads it from the snapshot); only its *semantic enum* moves to the game.

### Piece 3 — Raw input to the game

- The engine keeps draining the ring into `InputStateComponent` (existing camera/player/menu code untouched).
- During the same drain, the engine collects this tick's raw `InputEvent`s — **including `TextInput`** — into an engine-owned, frame-lifetime buffer.
- `GameState` gains a read-only view of that buffer (`const InputEvent* FrameInputEvents; size_t FrameInputEventCount;`), populated before `GameUpdate` and valid only for the current tick. The game iterates it to interpret text/keys however it wants.
- No `InputStateComponent` layout change is required (the raw view lives on `GameState`, host-owned), so this piece does not force an `ecs.dll` rebuild.

### Piece 4 — Registered serialization (generic save/load)

**Registry:** `world.RegisterComponentSerializer<T>(name, serialize, deserialize)` records, keyed by `std::type_index`:
- a stable string `name` (the `world.json` key),
- `serialize(const T&) -> json`,
- `deserialize(const json&) -> T`.

Built-ins register at `ecs.dll` init via an X-macro-driven block that wires up the existing `ComponentSerialization.h` functions. Game types register from `Game.dll` at startup (during `Uninitialized` seeding).

**Generic save/load via `IComponentArray` virtuals:** add to `IComponentArray`:
- `const char* Name() const`
- `bool HasFor(EntityId) const` (likely already present as `Has`)
- `void SerializeEntity(EntityId, json&) const`
- `void DeserializeEntity(EntityId, const json&)`
- singleton-aware variants as needed (the singleton entity is just an `EntityId`).

`ComponentArray<T>` implements these by calling the registered `serialize`/`deserialize` for `T` (set when the array is created or on first registration). `WorldManager` save/load iterate the array map generically — per entity, for each array that `HasFor(entity)`, write `{ Name(): SerializeEntity(...) }`; on load, for each json key, look up the array by name and `DeserializeEntity`. This **replaces** the explicit per-type lists in `WorldManager.cpp`.

**No immediate consumer.** The login form is runtime-only, so this piece is proven by a synthetic test: register a serializer for a test game component, save + reload a world, assert round-trip.

### Piece 5 — Editor inspector editing of game components

Game components must be inspectable/editable in the editor **without** `Game.dll` linking ImGui and **without** the editor knowing the game types.

- **Reflection via JSON.** The inspector renders any registered component by serializing it (Piece 4) to JSON and drawing a **generic JSON tree editor** (key → value widgets for numbers/strings/bools/nested objects/arrays) in `ImGuiRenderer`. Edits mutate the JSON.
- **Command path.** An edit produces an `ECSCommand` carrying `{ component name (string), json blob }` (string payloads cross the `ECSCommandRing` safely; no raw pointers, no `type_index` over the ring). `GameThread` applies it by looking up the registered deserializer by name and writing the component back. Add/remove-component commands also carry the component name. This **generalizes** `ApplyComponentCommand`/`RemoveComponentByType` — the explicit per-type branches collapse into a name→deserializer lookup. Built-ins go through the same path (their serializers are registered too).
- **Enumeration.** The inspector lists a selected entity's components generically by iterating the array map and asking each `IComponentArray` `Name()` + `HasFor(entity)`.
- Built-in components keep working through the same generic path; bespoke per-type ImGui widgets (e.g. transform gizmo fields) may remain as optional specializations layered over the generic JSON editor where richer UX is wanted, but are not required for correctness.

## Scope (deliberately out)

- **Renderer-visible-by-type game components** — impossible (the engine cannot name a `Game.dll`-only type; `type_index` identity is single-module). Not needed: the renderer reads built-in components (`UIRectComponent`/`TextComponent`/`TransformComponent`/`MeshComponent`/…) and game logic *drives* those via game components.
- **Cross-DLL sharing of a game-defined type between two non-engine modules** — out of scope; there is one game module.
- **Migrating existing input consumers** — Piece 3 is additive; camera/player/menu keep reading `InputStateComponent`.
- **The login UI itself** — separate downstream spec (appendix).

## Testing

- **`test_ecs` stays green**, plus new cases that define a component type **in the test TU** (outside `ecs.dll`) to prove header instantiation:
  - add / get / has / remove for the out-of-ecs.dll type;
  - snapshot + COW: mutate after `CreateSnapshot`, assert the snapshot is unchanged and the master diverges;
  - pool balance: the deleter recycles correctly across the module boundary (no leak/double-free).
- **Serialization round-trip** (Piece 4): register a test game component's (de)serializer, save a world to JSON and reload, assert equality. Verify built-ins still save/load identically (golden `world.json` compare or field-by-field).
- **Generic inspector command** (Piece 5): a unit-level test of the name→deserializer apply path (construct an `ECSCommand` with a name+json blob, process it, assert the component changed). Manual editor smoke: select an entity with a game component, edit a field, confirm the change applies through the ring.
- **States** (Piece 2): `test_ecs`/game build compiles with `GameStateComponent.Current` as `uint32_t`; UI scope filtering unchanged (manual smoke: menu vs in-level entity visibility).
- **Raw input** (Piece 3): a headless test feeding synthetic `InputEvent`s (incl `TextInput`) and asserting the game-side frame view exposes them in order.

## Build / test note

Build & test with the `msvc-win64-vs2026-community` preset only. Pieces 1, 2, 4, 5 touch `ECS.h`/`ecs.dll`/common → rebuild `ecs.dll` + editor + game and restart the editor **for these refactor commits** (the whole point is that *future* game components won't). No `GAME_API_VERSION` bump unless `game.h`'s exported surface changes. Commit identity: `Nuno Silva <nuno.levezinho@live.com.pt>`. Never `--no-verify`.

## Decisions locked

- Full extensibility: header-instantiable templates so `Game.dll` defines its own components/singletons; built-ins stay explicitly instantiated in `ecs.dll`.
- Game owns its states (`GameStateId` → game header; `GameStateComponent.Current` → `uint32_t`; engine compares the bit index opaquely).
- Raw input is exposed alongside the digested singleton (additive; existing consumers untouched).
- Game components are persistable now via a `RegisterComponentSerializer<T>` registry; save/load become generic over `IComponentArray`.
- Editor edits game components generically via JSON round-trip through the registered (de)serializers; `ECSCommands` dispatch becomes name-keyed. Piece 5 lands **before** the login-UI spec.

## Appendix — downstream Login UI spec (decisions already made, to expand later)

Captured so they survive into the next spec. The login UI is built **after** this boundary work and must be pure `Game.dll`.

- **Scope:** login only. Start Game button → login screen (username/password) → on submit, connect + run the existing auth→world→char flow (which already auto-advances world+char). No world/char screens.
- **Field fidelity:** real text entry — click a field to focus, type chars, Backspace deletes, Tab/click switches fields, password masked, submit on button or Enter. (Consumes Piece 3's raw text input.)
- **States:** add game-owned `Login` and `Connecting` states. `MainMenu` (Start Game) → `Login` (creds form) → `Connecting` (status text while the FSM runs) → `InLevel` on `InGame`. Failure → back to `Login` with error text. (Uses Piece 2.)
- **Form state:** a `LoginForm` **game singleton** (username/password buffers, focused field, status, error text) — a game-defined singleton enabled by Piece 1, runtime-only.
- **Gating:** servers still bind/listen on boot (always-up). Only `ClientSessionSystem` is gated: idle until login submit, then connects to auth with the typed `username`/`password` (`LoginReq`). On failure, surface error + return to `Login` rather than auto-retry.
- **Systems:** a new `LoginUISystem` (hit-tests button + fields reusing the `MenuInteractionSystem` pattern, consumes raw text, flips state on submit); `ClientSessionSystem` reads creds when it sees `Connecting`.
