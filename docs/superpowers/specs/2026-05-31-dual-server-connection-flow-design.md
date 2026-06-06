# Dual-server connection flow (Auth + World) — design

**Date:** 2026-05-31
**Branch:** `feat/auth-world-flow`
**Status:** IMPLEMENTED 2026-05-31 (commits `3b8bfff`..`1bf0790` on `feat/auth-world-flow`). Auto-tests green (`test_authflow` pure FSM; full regression). Four systems in `Game.dll`: `NetPumpSystem` (single shared-ring drain → per-adapter inboxes), `AuthServerSystem`, `WorldServerSystem`, `ClientSessionSystem` (both client handles + FSM + handoff + `GameState`→`InLevel`). **Key correction during build:** the shared inbound ring has ONE consumer, so multiple in-process polling systems cannibalize each other's events — fixed with a single `NetPumpSystem` (Input phase) fanning events per-adapter; the systems read their own inbox. **GUI smoke confirmed 2026-06-06** (editor walks ConnectingAuth → … → IN GAME, both listeners bound, handoff to 27101, `GameState`→`InLevel`). Deferred per spec: real auth/UI/persistence, dedicated-process split + role gating.

## Goal

Prototype the ARPG connection topology: an **AuthServer** (login + world selection) and a **WorldServer** (character selection + play state), with **two TCP clients** in the game and a **client-side session state machine** that walks `auth → world-select → (handoff) → char-select → in-game`. AuthServer owns auth + world selection; WorldServer owns character selection + entering play.

**Prototype fidelity = stubbed.** Prove the *topology + flow + handoff + state machine* end to end over real loopback TCP. Auth accepts any credentials; world/char lists are hardcoded; the flow **auto-advances** (auto-login, auto-pick world[0], auto-pick char[0]) and is observed via logs + the game-state transition to `InLevel`. No UI screens, no persistence, no real auth.

## Process model (this prototype)

All four systems live in `Game.dll` and run **together in one process** (the editor) over loopback — real netlib TCP on two ports. No dedicated `server.exe` / `AppRole` split and **no role gating** for now; the four systems run unconditionally. The dedicated-process split (servers in `server.exe`, role/kind gating, launching) is the explicit **later phase**, out of scope here.

This replaces the current Ping/Pong demo (`NetClientSystem` / `NetServerSystem` in `game.cpp`). A Ping/Pong heartbeat may be retained on the world client *after* reaching in-game (optional).

## Context (as-built, reused)

- **Transport:** `NetServices` (`src/common/include/NetServices.h`) — `CreateServer`/`CreateClient` (return `NetHandle`; the game can hold several), `AcquireSend`/`Send`/`AbortSend`, opaque `PollEvent`, `Close`. Engine wire = `[u32 len][opaque]`; the game owns a `[u16 opcode][protobuf]` payload.
- **Codec:** `WireCodec` (`src/game/src/WireCodec.h`) — `EncodeInto`/`PeekOpcode`/`Decode`/`OpcodeOf<M>` (specialize one line per message). `SendMessage<M>` glue lives in `game.cpp`.
- **Schema:** `src/game/proto/wire.proto` (proto3, LITE_RUNTIME, `package wire`). Currently Ping/Pong/Snapshot.
- **App state:** `GameStateId` (`MainMenu`/`InLevel`/…) + `GameStateComponent` singleton; `AppFlowSystem` owns transitions. The session FSM flips `GameStateComponent` → `InLevel` on reaching in-game.
- **Caps (just landed):** `ConnConfig.maxFrameBytes` (256 KiB) + `maxSendQueueBytes` (1 MiB); all flow messages are tiny, well under both.

## Components

Four `ISystem`s registered in `GameRegisterSystems` (replacing the Ping/Pong demo systems):

### `AuthServerSystem`
- On first tick: `CreateServer` bound to `127.0.0.1:kAuthPort` (`27100`), `gameResident = true`.
- Per-conn state: `authed` bool (keyed by `NetConnId`).
- Handlers (dispatch on `PeekOpcode`):
  - `LoginReq` → accept any → mark conn authed → reply `LoginResp{ok=true}`.
  - `WorldListReq` → reply `WorldListResp` with the hardcoded world list (each `WorldInfo{id, name, host="127.0.0.1", port=kWorldPort}`).
  - `WorldSelectReq{world_id}` → reply `WorldSelectResp{ok=true, host="127.0.0.1", port=kWorldPort, session_token="sess-<counter>"}`. (Token is a stub string; not validated cryptographically.)
- `Disconnected`/`Error` → drop the per-conn entry.

### `WorldServerSystem`
- On first tick: `CreateServer` bound to `127.0.0.1:kWorldPort` (`27101`), `gameResident = true`.
- Per-conn state: `sessionValid` bool + `selectedChar` id.
- Handlers:
  - `SessionAuthReq{session_token}` → accept any non-empty token (stub) → mark `sessionValid` → reply `SessionAuthResp{ok=true}`.
  - `CharListReq` → reply `CharListResp` with hardcoded chars (`CharInfo{id, name, level}`).
  - `CharSelectReq{char_id}` → record `selectedChar` → reply `CharSelectResp{ok=true}`.
  - `EnterGameReq` → reply `EnterGameResp{ok=true}`.
- `Disconnected`/`Error` → drop the per-conn entry.

### `ClientSessionSystem` (owns BOTH client handles + the FSM)
- Owns `NetHandle m_AuthClient`, `NetHandle m_WorldClient` (the two clients), a `SessionState`, and stashed flow data (chosen world `host:port`, `session_token`).
- Drives the state machine (below). Only one client handle is active at a time; the handoff closes the auth client and opens the world client.
- Bounded retry on connect failure / disconnect (mirrors the existing client retry: fast then slow), resetting to `Disconnected`.

## Client session state machine

```
Disconnected
 → ConnectingAuth     CreateClient → 127.0.0.1:kAuthPort
 → Authenticating     on auth Connected: SendMessage(LoginReq{user,pass})
 → WorldSelecting      on LoginResp.ok: send WorldListReq
                       on WorldListResp: pick worlds[0] → send WorldSelectReq{id}
 → WorldHandoff        on WorldSelectResp{ok,host,port,token}: stash {host,port,token};
                       Close(m_AuthClient); CreateClient(m_WorldClient) → host:port
 → SessionAuthing      on world Connected: SendMessage(SessionAuthReq{token})
 → CharSelecting       on SessionAuthResp.ok: send CharListReq
                       on CharListResp: pick chars[0] → send CharSelectReq{id}
 → EnteringGame        on CharSelectResp.ok: send EnterGameReq
 → InGame              on EnterGameResp.ok: GameStateComponent → InLevel; (optional) start heartbeat
```

- Every transition `SM_TRACE`d (`SessionState` → name).
- `LoginResp.ok == false` / any `*.ok == false` → log `SM_WARN` + reset to `Disconnected` (retry).
- Disconnect/Error on the active client at any non-terminal step → log + reset to `Disconnected`.
- Receive dispatch: `PeekOpcode(ev.payload, ev.len)` then `Decode<M>(ev.payload+2, ev.len-2, …)`; ignore an opcode that doesn't match the current state's expected reply.

## Protocol (`wire.proto` additions)

Append opcodes to the `Opcode` enum (keep Ping=1/Pong=2/Snapshot=3) and add paired messages:

```proto
enum Opcode {
  // ... existing 0-3 ...
  OPCODE_LOGIN_REQ          = 10;  OPCODE_LOGIN_RESP          = 11;
  OPCODE_WORLD_LIST_REQ     = 12;  OPCODE_WORLD_LIST_RESP     = 13;
  OPCODE_WORLD_SELECT_REQ   = 14;  OPCODE_WORLD_SELECT_RESP   = 15;
  OPCODE_SESSION_AUTH_REQ   = 16;  OPCODE_SESSION_AUTH_RESP   = 17;
  OPCODE_CHAR_LIST_REQ      = 18;  OPCODE_CHAR_LIST_RESP      = 19;
  OPCODE_CHAR_SELECT_REQ    = 20;  OPCODE_CHAR_SELECT_RESP    = 21;
  OPCODE_ENTER_GAME_REQ     = 22;  OPCODE_ENTER_GAME_RESP     = 23;
}

message LoginReq        { string username = 1; string password = 2; }
message LoginResp       { bool ok = 1; string reason = 2; }
message WorldInfo       { uint32 id = 1; string name = 2; string host = 3; uint32 port = 4; }
message WorldListReq    {}
message WorldListResp   { repeated WorldInfo worlds = 1; }
message WorldSelectReq  { uint32 world_id = 1; }
message WorldSelectResp { bool ok = 1; string host = 2; uint32 port = 3; string session_token = 4; }
message SessionAuthReq  { string session_token = 1; }
message SessionAuthResp { bool ok = 1; }
message CharInfo        { uint32 id = 1; string name = 2; uint32 level = 3; }
message CharListReq     {}
message CharListResp    { repeated CharInfo chars = 1; }
message CharSelectReq   { uint32 char_id = 1; }
message CharSelectResp  { bool ok = 1; }
message EnterGameReq    {}
message EnterGameResp   { bool ok = 1; }
```

Add one `OpcodeOf<M>` specialization per message in `WireCodec.h`. `port` is `uint32` on the wire, cast to `uint16_t` for the endpoint.

## Stub data (hardcoded constants in `game.cpp`)

- Worlds: e.g. `{1,"Local World",127.0.0.1,27101}` (one is enough; two proves list handling).
- Chars: e.g. `{1,"Hero",10}`, `{2,"Rogue",7}`.
- Ports: `kAuthPort = 27100`, `kWorldPort = 27101` (file-local constants; config later).
- Session token: `"sess-" + counter` on the AuthServer; WorldServer accepts any non-empty token.

## Testing

- **FSM unit test** — extract the transition logic into a pure, socket-free function/table (input: current `SessionState` + received opcode/ok; output: next `SessionState` + action). Unit-test the full happy path + a failure (`LoginResp.ok=false` → Disconnected) without sockets. (`tests/test_authflow.cpp`.)
- **End-to-end in-process test** — drive the real path through `NetServices`: stand up the two servers + run the client FSM to `InGame` over loopback; assert it reaches `InGame` and the handoff hit port B. Mirrors `tests/test_net.cpp` style (it already exercises NetSubsystem round-trips). Bounded with a timeout.
- **Manual smoke** — run the editor; watch the log walk `ConnectingAuth → … → InGame`, both listeners bound, the handoff reconnect to `27101`, and `GameStateComponent` flip to `InLevel`.

## Out of scope (deliberate)

- Real credentials / auth, password hashing, account storage, persistence/DB.
- TLS / encryption / wire security beyond the existing caps.
- Multiple concurrent players, real character/world data, real spawn/replication.
- The dedicated-process split (`server.exe` Auth/World roles, role gating, launching both) — explicit **next phase**.
- UI screens for login/world/char (auto-advance now; clickable screens via the menu/StateScope system are a follow-up).
- Robust reconnection/resume beyond basic bounded retry.

## Decisions locked

- Stubbed fidelity; auto-advance (no UI); single process (editor); real loopback TCP on two ports.
- Four systems in `Game.dll`: `AuthServerSystem`, `WorldServerSystem`, `ClientSessionSystem` (owns both client handles + FSM). Replaces the Ping/Pong demo systems.
- Handoff: AuthServer returns `{host, port, session_token}`; client closes the auth client and opens the world client to that address, presenting the token.
- Protocol via the existing `[u16 opcode][protobuf]` framing + `WireCodec`; new opcodes 10–23.
- Auth accepts any creds; WorldServer accepts any non-empty token; hardcoded world/char lists.

## Build / test note

Build & test with the `msvc-win64-vs2026-community` preset only. New `wire.proto` messages trigger `protoc` codegen into `Game.dll` (a `.cpp`-only / codegen change — no `Game.h` / `GAME_API_VERSION` bump; hot-reloads). Commit identity: `Nuno Silva <nuno.levezinho@live.com.pt>`.
