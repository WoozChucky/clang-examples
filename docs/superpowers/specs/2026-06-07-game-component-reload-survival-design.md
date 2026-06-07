# Game-Component Hot-Reload Survival — Design

**Date:** 2026-06-07
**Status:** Approved (brainstorm) — ready for implementation plan

## Problem

Game-defined (non-builtin) ECS component arrays are **cleared on every `Game.dll` hot-reload**
(`ECS::RemoveNonBuiltinComponentArrays()` at `src/engine/src/threading/GameThread.cpp:227`). This is
mandatory for safety: a `ComponentArray<GameType>` carries destructor/vtable/template code compiled
into `Game.dll`; those objects **must** be destroyed while the DLL is still mapped, or a later
dtor/virtual call after `FreeLibrary` is a dangling-code access violation.

Consequence today: any entity holding a game-defined component **loses it** across a hot-reload. There
is no way for a game-owned component to be both (a) truly owned by `Game.dll` and (b) preserved across
reload. The two existing options are both unsatisfactory:

- **Scenario A** — keep the struct in `ECS.h` (engine-defined) and merely flag its serializer
  non-builtin: survives reload, but it is not game-owned (engine must link the type).
- **Scenario B** — move the struct into `Game.dll`: game-owned, but cleared on every reload.

Goal: a mechanism where a game-defined component is owned by `Game.dll` **and** its entities retain
their component values across hot-reload.

## Key insight: extract → reload → restore

Entities are **not** destroyed across a reload — only component *arrays* are dropped (`EntityStore` is
untouched). So the data can be:

1. **Extracted** into DLL-neutral, host-owned storage **before** `FreeLibrary` (while the old DLL is
   still mapped, so its extraction code can run), then
2. **Restored** onto the same `EntityId`s **after** the new DLL has re-registered its types.

Extraction and restoration occur in the **same scope** in `GameThread.cpp` (~lines 227–236), so the
preserved blob is a local variable. It holds only `std`/`nlohmann::json` data, which lives in the
shared CRT heap (not in `Game.dll`), so it survives `FreeLibrary` safely.

## Disk-persistence ≠ reload-preservation

These are deliberately **separate** concerns and use **separate** code paths:

- **Disk persistence** (`world.json`, via `SaveEntityComponents`/`LoadEntityComponents`) uses the
  serializer `save`/`load` (json) only. A POD component with no `to_json` is **not** written to
  `world.json` — the developer did not opt into disk persistence, and we must not leak opaque binary
  blobs into the human-editable scene file.
- **Reload preservation** (this feature) may use **either** the json serializer **or** a raw-byte
  (`memcpy`) path, chosen per component type.

This separation is the reason we do **not** fold the byte path into the json `save`/`load`.

## Preservation strategies (auto-selected at registration)

Each registered component gets, at `Register<T>` time:

1. **Serializer (json)** — installed when `T` has ADL `to_json`/`from_json`. Reuses the existing
   `save`/`load` function pointers. Used for complex types (heap members: `std::string`,
   `std::vector`, etc.) and for disk persistence.
2. **Byte (memcpy)** — installed when `std::is_trivially_copyable_v<T>`. **No hand-written serializer
   required.** New function pointers extract/ingest `sizeof(T)` bytes.
3. **Neither** — non-trivially-copyable `T` with no `to_json`: both strategies null. Compiles fine;
   flagged at reload time as unpreservable.

**Reload chooses:** byte path if present (lossless, fast, no float→json precision concerns), else json
path, else warn + clear. A POD type that *also* has `to_json` gets both (json for disk, byte for
reload) — reload prefers byte.

## Scope: opt-in via registration

"Preserve all non-builtin components" means, in practice, **all registered non-builtin components**.
Preservation is opt-in by calling `SerializerRegistry().Register<T>("Name")` (one line). An
unregistered game component is cleared silently on reload (current behavior) — these are almost always
per-tick scratch that self-heals on the next tick.

## Components

### 1. `ComponentSerializerRegistry` / `ComponentSerializerEntry` (`src/common/include/ComponentSerializerRegistry.h`)

- `Register<T>(name, builtin = false)` becomes `if constexpr`-branched:
  - install json `save`/`load` **only when** `to_json` exists (today it is unconditional, which would
    fail to compile for a POD-without-`to_json`);
  - install new byte fn-ptrs when `std::is_trivially_copyable_v<T>`;
  - if neither applies, leave both null.
- New entry members (reload-preservation only; distinct from disk `save`/`load`):
  ```cpp
  void (*reloadExtract)(const ECS&, EntityId, std::vector<std::byte>&) = nullptr; // memcpy out
  void (*reloadIngest)(ECS&, EntityId, const std::vector<std::byte>&)   = nullptr; // memcpy in + AddComponent
  ```
- Existing builtins (all have `to_json`) are unaffected — backward-compatible.

### 2. Preserve/restore helpers (`src/ecs/src/ComponentSerializers.cpp`, exported from `ecs.dll`)

```cpp
struct PreservedComponent {
    std::string name;
    EntityId    entity;
    bool        useBytes;            // true → bytes, false → json
    nlohmann::json json;             // serializer path
    std::vector<std::byte> bytes;    // byte path
};

ECS_API std::vector<PreservedComponent> PreserveNonBuiltinComponents(const ECS& world);
ECS_API void RestoreNonBuiltinComponents(ECS& world, const std::vector<PreservedComponent>& blob);
```

- `PreserveNonBuiltinComponents`: iterate **registered non-builtin entries** × `world.GetActiveEntities()`;
  for each present component (`entry.has`), dispatch byte (preferred) or json; collect. Components with
  neither strategy → `SM_WARN(... not preservable ...)` and skip (they will be cleared).
- `RestoreNonBuiltinComponents`: for each `PreservedComponent`, look up the (freshly re-registered)
  entry by name and call `reloadIngest` (byte) or `load` (json) onto the same entity. A name absent
  from the registry after reload (game stopped registering it) → `SM_WARN` and skip.

### 3. Reload barrier (`src/engine/src/threading/GameThread.cpp`)

At the reload barrier (~line 227), replace the bare clear with extract → clear → reload → restore:

```cpp
auto preserved = PreserveNonBuiltinComponents(gameState.World);   // DLL mapped
gameState.World.RemoveNonBuiltinComponentArrays();                // unchanged
m_AppContext->LatestWorldSnapshot.store(gameState.World.CreateSnapshot(), ...); // builtin-only
if (m_GameLib.LoadOrReload("Game.dll", &gameState)) { ... }       // FreeLibrary + new DLL + re-register
RestoreNonBuiltinComponents(gameState.World, preserved);          // new-DLL code, same entities
m_AppContext->LatestWorldSnapshot.store(gameState.World.CreateSnapshot(), ...); // publish restored
```

The shutdown path (~line 587) stays **clear-only** — there is no subsequent reload to restore into.

### 4. Game registration hook (`src/game`, `src/engine/src/threading/GameLibrary.cpp`, `src/game/include/game.h`)

- New export `GameRegisterComponents()` (mirrors `GameRegisterSystems`), declared in `game.h`,
  implemented in the game, resolved + called by `GameLibrary::LoadOrReload` **after** the module loads
  and **before** `RestoreNonBuiltinComponents` runs.
- The game implements it as one `SerializerRegistry().Register<T>("Name")` per game-owned component.
  `Register` upsert (already reload-safe) replaces stale fn-ptrs left by the unloaded DLL.
- New export changes the ABI → **bump `GAME_API_VERSION` (21 → 22)**, rebuild `ecs` + `engine` +
  `game`, restart the editor.

## Data flow

```
[GameThread reload barrier, Game.dll still mapped]
  PreserveNonBuiltinComponents(world)
    for entry in registry where !entry.builtin:
      for e in world.GetActiveEntities():
        if entry.has(world, e):
          if entry.reloadExtract:  bytes  → PreservedComponent{useBytes=true}
          elif entry.save (json):  json   → PreservedComponent{useBytes=false}
          else:                    SM_WARN + skip
  RemoveNonBuiltinComponentArrays()        // destroy arrays (DLL mapped)
  publish builtin-only snapshot
[FreeLibrary old → LoadLibrary new]
  GameRegisterSystems(scheduler)           // existing
  GameRegisterComponents()                 // NEW — upsert registry entries for game types
[restore]
  RestoreNonBuiltinComponents(world, preserved)
    for pc in preserved:
      entry = registry.Find(pc.name)
      if !entry: SM_WARN + skip
      elif pc.useBytes: entry.reloadIngest(world, pc.entity, pc.bytes)
      else:             entry.load(world, pc.entity, pc.json)
  publish restored snapshot
```

## Error handling

- **Unpreservable registered component** (non-trivially-copyable, no serializer):
  `SM_WARN("<Name> not preservable (non-trivially-copyable, no serializer) — dropped on reload")`;
  cleared. (Choice A: lenient warn-on-reload, never blocks the reload.)
- **Unregistered game component**: cleared silently (current behavior; preservation is opt-in).
- **Name registered before reload but absent after** (game removed its registration): `SM_WARN` on
  restore; the data for that name is dropped.
- **Entity no longer valid** at restore (should not happen — entities survive reload): guard with
  `world.IsValidEntity` and skip + `SM_WARN` defensively.

## Testing

- **`tests/test_compserial.cpp`** (extend): `Register<T>` installs the byte fn-ptrs for a
  trivially-copyable probe and json `save`/`load` for a complex probe; a non-trivially-copyable,
  no-`to_json` probe registers with both strategies null.
- **`tests/test_reloadpreserve.cpp`** (new, links `ecs` + `nlohmann`): build an `ECS` with a POD game
  component (byte path) and a complex game component (json path) on several entities; call
  `PreserveNonBuiltinComponents` → `RemoveNonBuiltinComponentArrays` → re-`Register` →
  `RestoreNonBuiltinComponents`; assert component values and `EntityId` identity survive; assert the
  unpreservable case warns and drops; assert builtins are untouched by the non-builtin preserve/restore.
- Manual smoke (human-owned): add a real game component + `GameRegisterComponents` registration, place
  it on an entity, edit `Game.cpp`, rebuild `game`, confirm the component (and its values) survive the
  hot-reload in the editor.

## Out of scope

- Preserving unregistered game components (no opt-in → no preservation).
- Cross-layout migration (a struct whose fields changed between the old and new DLL): the byte path
  assumes identical layout across the reload (true for a `.cpp`-only edit; a struct-layout change
  already requires `GAME_API_VERSION` bump + editor restart, which discards the running world anyway).
- Disk persistence changes — `world.json` save/load is unchanged.

## Done criteria

- A game-owned, `Game.dll`-defined component placed on an entity **retains its value across a
  `Game.cpp`-only hot-reload**, given a one-line `Register<T>` in `GameRegisterComponents`.
- POD game components need **no** hand-written serializer (byte path); complex ones use their existing
  `to_json`/`from_json`.
- Unpreservable registered components warn and clear (no silent corruption); disk `world.json` is
  unaffected; builtins continue to survive natively.
- `test_compserial`, `test_reloadpreserve`, `test_ecs`, `test_worldserial` green.
```
