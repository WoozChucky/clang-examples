# Dual-server connection flow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prototype the ARPG connection topology — an in-process AuthServer (login + world select) and WorldServer (char select + enter-game), two TCP clients, and a client session state machine that walks `auth → world-select → (handoff) → char-select → in-game` over real loopback TCP, auto-advancing with stub data.

**Architecture:** Four `ISystem`s in `Game.dll` running together in the editor process: `AuthServerSystem` (binds 127.0.0.1:27100), `WorldServerSystem` (binds 127.0.0.1:27101), and `ClientSessionSystem` (owns both client `NetHandle`s + the FSM). The FSM lives in a pure, socket-free header (`SessionFlow.h`) so it's unit-testable; the system performs the actual `NetServices` calls per FSM action. Protocol is the existing `[u16 opcode][protobuf]` framing + `WireCodec`, with new messages in `wire.proto`.

**Tech Stack:** C++23, `NetServices`/`netlib` (loopback TCP), protobuf-lite (`wire.proto` → `wire.pb.*` into `Game.dll`), `WireCodec`, the game's `GameStateId`/`GameStateComponent` app-state, CMake preset `msvc-win64-vs2026-community`.

**Spec:** `docs/superpowers/specs/2026-05-31-dual-server-connection-flow-design.md`

**Conventions:**
- Build/test ONLY with `msvc-win64-vs2026-community`. Configure: `cmake --preset msvc-win64-vs2026-community`. Build a target: `cmake --build --preset msvc-win64-vs2026-community --target <t>`. Test exes run from `out/build/msvc-win64-vs2026-community/bin/Debug/`.
- Commit as `git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "..."`. NEVER `--no-verify`. Do NOT push. Stay on branch `feat/auth-world-flow` (already checked out).
- **Git path-case gotcha:** stage with the exact case `git status` shows (the repo tracks `src/game/src/game.cpp`, `src/game/proto/wire.proto` — all lowercase). After committing run `git status --porcelain` (clean) + `git show HEAD --stat`.
- `.proto`/`.cpp`-only changes hot-reload; no `GAME_API_VERSION` bump. Editing `wire.proto` auto-reruns `protoc` (the codegen `DEPENDS wire.proto`).
- The game has **no unit-test harness**; its runtime self-check is the boot-smoke block in `game.cpp` (the `GameStateId::Uninitialized` branch). The pure FSM gets a real unit test (`test_authflow`); servers/client are verified by build + the boot smoke + manual smoke.

---

## File Structure

**New files:**
- `src/game/src/SessionFlow.h` — pure, socket-free client session FSM: `SessionState`, `SessionInput`, `SessionAction`, `SessionStep`, `AdvanceSession()`. No protobuf, no `NetServices`. Header-only, game-local.
- `tests/test_authflow.cpp` — unit tests for `AdvanceSession` (happy path + failure).

**Modified files:**
- `src/game/proto/wire.proto` — append opcodes 10–23 + the req/resp messages.
- `src/game/src/WireCodec.h` — add `OpcodeOf<M>` specializations for the new messages.
- `src/game/src/game.cpp` — extend the boot-smoke round-trip; add `AuthServerSystem`, `WorldServerSystem`, `ClientSessionSystem`; remove the Ping/Pong `NetServerSystem`/`NetClientSystem` demo systems; update `GameRegisterSystems`.
- `tests/CMakeLists.txt` — add the `test_authflow` target.

---

## Task 1: Protocol — wire.proto messages + opcodes + WireCodec + boot-smoke round-trip

**Files:**
- Modify: `src/game/proto/wire.proto`
- Modify: `src/game/src/WireCodec.h`
- Modify: `src/game/src/game.cpp` (boot-smoke block)

- [ ] **Step 1: Append opcodes + messages to `src/game/proto/wire.proto`** (after the existing `Snapshot` message; extend the `Opcode` enum in place)

Add to the `Opcode` enum (keep the existing `OPCODE_UNSPECIFIED=0`, `PING=1`, `PONG=2`, `SNAPSHOT=3`):
```proto
  OPCODE_LOGIN_REQ          = 10;
  OPCODE_LOGIN_RESP         = 11;
  OPCODE_WORLD_LIST_REQ     = 12;
  OPCODE_WORLD_LIST_RESP    = 13;
  OPCODE_WORLD_SELECT_REQ   = 14;
  OPCODE_WORLD_SELECT_RESP  = 15;
  OPCODE_SESSION_AUTH_REQ   = 16;
  OPCODE_SESSION_AUTH_RESP  = 17;
  OPCODE_CHAR_LIST_REQ      = 18;
  OPCODE_CHAR_LIST_RESP     = 19;
  OPCODE_CHAR_SELECT_REQ    = 20;
  OPCODE_CHAR_SELECT_RESP   = 21;
  OPCODE_ENTER_GAME_REQ     = 22;
  OPCODE_ENTER_GAME_RESP    = 23;
```
Append these messages at the end of the file:
```proto
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

- [ ] **Step 2: Add `OpcodeOf<M>` specializations in `src/game/src/WireCodec.h`** (after the existing Ping/Pong/Snapshot specializations, before `EncodeInto`)

```cpp
template <> inline wire::Opcode OpcodeOf<wire::LoginReq>()        { return wire::OPCODE_LOGIN_REQ; }
template <> inline wire::Opcode OpcodeOf<wire::LoginResp>()       { return wire::OPCODE_LOGIN_RESP; }
template <> inline wire::Opcode OpcodeOf<wire::WorldListReq>()    { return wire::OPCODE_WORLD_LIST_REQ; }
template <> inline wire::Opcode OpcodeOf<wire::WorldListResp>()   { return wire::OPCODE_WORLD_LIST_RESP; }
template <> inline wire::Opcode OpcodeOf<wire::WorldSelectReq>()  { return wire::OPCODE_WORLD_SELECT_REQ; }
template <> inline wire::Opcode OpcodeOf<wire::WorldSelectResp>() { return wire::OPCODE_WORLD_SELECT_RESP; }
template <> inline wire::Opcode OpcodeOf<wire::SessionAuthReq>()  { return wire::OPCODE_SESSION_AUTH_REQ; }
template <> inline wire::Opcode OpcodeOf<wire::SessionAuthResp>() { return wire::OPCODE_SESSION_AUTH_RESP; }
template <> inline wire::Opcode OpcodeOf<wire::CharListReq>()     { return wire::OPCODE_CHAR_LIST_REQ; }
template <> inline wire::Opcode OpcodeOf<wire::CharListResp>()    { return wire::OPCODE_CHAR_LIST_RESP; }
template <> inline wire::Opcode OpcodeOf<wire::CharSelectReq>()   { return wire::OPCODE_CHAR_SELECT_REQ; }
template <> inline wire::Opcode OpcodeOf<wire::CharSelectResp>()  { return wire::OPCODE_CHAR_SELECT_RESP; }
template <> inline wire::Opcode OpcodeOf<wire::EnterGameReq>()    { return wire::OPCODE_ENTER_GAME_REQ; }
template <> inline wire::Opcode OpcodeOf<wire::EnterGameResp>()   { return wire::OPCODE_ENTER_GAME_RESP; }
```

- [ ] **Step 3: Extend the boot-smoke round-trip in `game.cpp`** to cover a new message with a repeated field (proves codegen + WireCodec on the new types). Find the existing boot-smoke block (the `GameStateId::Uninitialized` branch with the `protobuf EncodeInto/PeekOpcode/Decode round-trip` log). After the existing Ping round-trip check, add:

```cpp
        // New flow messages round-trip (incl. a repeated field).
        wire::WorldListResp wl;
        auto* w = wl.add_worlds();
        w->set_id(1); w->set_name("Local World"); w->set_host("127.0.0.1"); w->set_port(27101);
        uint8_t wbuf[128];
        const uint32_t wn = wirecodec::EncodeInto(wbuf, sizeof(wbuf), wl);
        wire::WorldListResp wlBack;
        const bool wlOk = wn >= 2
            && wirecodec::PeekOpcode(wbuf, wn) == static_cast<uint16_t>(wire::OPCODE_WORLD_LIST_RESP)
            && wirecodec::Decode(wbuf + 2, wn - 2, wlBack)
            && wlBack.worlds_size() == 1 && wlBack.worlds(0).port() == 27101;
        if (wlOk) SM_TRACE("[GAMEDLL] flow-proto round-trip OK (WorldListResp)");
        else      SM_ERROR("[GAMEDLL] flow-proto round-trip FAILED");
```

- [ ] **Step 4: Build the game target** (triggers `protoc` regen of `wire.pb.*` + compiles the new `OpcodeOf` specializations + the smoke block)

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target game
```
Expected: clean build; `Game.dll` produced. (The boot-smoke logs `flow-proto round-trip OK` at editor/runtime startup — verified live in Task 5's manual smoke.)

- [ ] **Step 5: Commit**

```bash
git add src/game/proto/wire.proto src/game/src/WireCodec.h src/game/src/game.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(net): wire.proto auth/world/char flow messages + WireCodec opcodes"
```

---

## Task 2: SessionFlow — pure client FSM + unit test

**Files:**
- Create: `src/game/src/SessionFlow.h`
- Create: `tests/test_authflow.cpp`
- Modify: `tests/CMakeLists.txt`

This is the testable core: a pure transition function with no protobuf, no sockets. The `ClientSessionSystem` (Task 4) calls it and performs the I/O the returned `SessionAction` names.

- [ ] **Step 1: Write the failing test** — `tests/test_authflow.cpp`

```cpp
#include <cassert>
#include <cstdio>
#include "SessionFlow.h"

using S = SessionState; using I = SessionInput; using A = SessionAction;

static int g_Failures = 0;
#define CHECK(cond, msg) do { if(!(cond)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,msg); ++g_Failures; } } while(0)

static void expect(S from, I in, S nextWant, A actWant, const char* tag) {
    SessionStep st = AdvanceSession(from, in);
    CHECK(st.next == nextWant && st.action == actWant, tag);
}

int main() {
    // Happy path, step by step.
    expect(S::ConnectingAuth, I::AuthConnected,     S::Authenticating, A::SendLogin,        "connected->login");
    expect(S::Authenticating, I::LoginOk,           S::WorldSelecting, A::SendWorldListReq, "loginok->worldlistreq");
    expect(S::WorldSelecting, I::WorldListReceived, S::WorldSelecting, A::SendWorldSelect,  "worldlist->select");
    expect(S::WorldSelecting, I::WorldSelectOk,     S::WorldHandoff,   A::BeginHandoff,     "selectok->handoff");
    expect(S::WorldHandoff,   I::WorldConnected,    S::SessionAuthing, A::SendSessionAuth,  "worldconn->sessionauth");
    expect(S::SessionAuthing, I::SessionAuthOk,     S::CharSelecting,  A::SendCharListReq,  "sessok->charlistreq");
    expect(S::CharSelecting,  I::CharListReceived,  S::CharSelecting,  A::SendCharSelect,   "charlist->select");
    expect(S::CharSelecting,  I::CharSelectOk,      S::EnteringGame,   A::SendEnterGame,    "charselok->entergame");
    expect(S::EnteringGame,   I::EnterGameOk,       S::InGame,         A::EnterGame,        "entergameok->ingame");

    // Failures + drops anywhere reset to Disconnected.
    expect(S::Authenticating, I::LoginFail,         S::Disconnected,  A::Reset, "loginfail->reset");
    expect(S::SessionAuthing, I::SessionAuthFail,   S::Disconnected,  A::Reset, "sessfail->reset");
    expect(S::WorldHandoff,   I::Dropped,           S::Disconnected,  A::Reset, "drop->reset");
    expect(S::CharSelecting,  I::CharSelectFail,    S::Disconnected,  A::Reset, "charfail->reset");

    // Unexpected input for a state is ignored (no spurious transition).
    expect(S::Authenticating, I::CharListReceived,  S::Authenticating, A::None, "unexpected->none");

    if (g_Failures == 0) { std::printf("All authflow tests passed.\n"); return 0; }
    std::printf("%d authflow test(s) FAILED.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 2: Run to verify it fails** (no `SessionFlow.h` yet)

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_authflow
```
Expected: COMPILE error (target/`SessionFlow.h` missing) — after Step 4 adds the target it will compile-fail on the missing header until Step 3.

- [ ] **Step 3: Create `src/game/src/SessionFlow.h`**

```cpp
#pragma once
#include <cstdint>

// Pure, socket-free client session state machine for the dual-server connection flow.
// ClientSessionSystem feeds it inputs (replies/events) and performs the I/O named by the
// returned action. No protobuf / NetServices dependency, so it is trivially unit-testable.

enum class SessionState : uint8_t {
    Disconnected,     // no active client; system (re)creates the auth client -> ConnectingAuth
    ConnectingAuth,   // auth client connecting
    Authenticating,   // auth connected; login sent, awaiting LoginResp
    WorldSelecting,   // logged in; requesting world list, then selecting
    WorldHandoff,     // world selected; closing auth client, opening world client
    SessionAuthing,   // world connected; session token sent, awaiting SessionAuthResp
    CharSelecting,    // session valid; requesting char list, then selecting
    EnteringGame,     // char selected; enter-game sent, awaiting EnterGameResp
    InGame,           // terminal: flip GameStateComponent -> InLevel
};

enum class SessionInput : uint8_t {
    AuthConnected,
    LoginOk, LoginFail,
    WorldListReceived,           // system already picked worlds[0]
    WorldSelectOk, WorldSelectFail,
    WorldConnected,
    SessionAuthOk, SessionAuthFail,
    CharListReceived,            // system already picked chars[0]
    CharSelectOk, CharSelectFail,
    EnterGameOk, EnterGameFail,
    Dropped,                     // disconnect/error on the active client
};

enum class SessionAction : uint8_t {
    None,
    SendLogin,
    SendWorldListReq,
    SendWorldSelect,
    BeginHandoff,                // Close(auth client); Create world client to the returned host:port
    SendSessionAuth,
    SendCharListReq,
    SendCharSelect,
    SendEnterGame,
    EnterGame,                   // flip game state to InLevel
    Reset,                       // -> Disconnected (retry the whole flow)
};

struct SessionStep { SessionState next; SessionAction action; };

// Reply-driven transitions for the CONNECTED states (ConnectingAuth onward). The
// Disconnected -> ConnectingAuth step (creating the auth client) is driven by the system,
// not this function. Any failure/drop resets to Disconnected.
inline SessionStep AdvanceSession(SessionState s, SessionInput in) {
    using S = SessionState; using I = SessionInput; using A = SessionAction;

    switch (in) {
        case I::Dropped:
        case I::LoginFail:
        case I::WorldSelectFail:
        case I::SessionAuthFail:
        case I::CharSelectFail:
        case I::EnterGameFail:
            return { S::Disconnected, A::Reset };
        default: break;
    }

    switch (s) {
        case S::ConnectingAuth:
            if (in == I::AuthConnected)     return { S::Authenticating, A::SendLogin };
            break;
        case S::Authenticating:
            if (in == I::LoginOk)           return { S::WorldSelecting, A::SendWorldListReq };
            break;
        case S::WorldSelecting:
            if (in == I::WorldListReceived) return { S::WorldSelecting, A::SendWorldSelect };
            if (in == I::WorldSelectOk)     return { S::WorldHandoff,   A::BeginHandoff };
            break;
        case S::WorldHandoff:
            if (in == I::WorldConnected)    return { S::SessionAuthing, A::SendSessionAuth };
            break;
        case S::SessionAuthing:
            if (in == I::SessionAuthOk)     return { S::CharSelecting,  A::SendCharListReq };
            break;
        case S::CharSelecting:
            if (in == I::CharListReceived)  return { S::CharSelecting,  A::SendCharSelect };
            if (in == I::CharSelectOk)      return { S::EnteringGame,   A::SendEnterGame };
            break;
        case S::EnteringGame:
            if (in == I::EnterGameOk)       return { S::InGame,         A::EnterGame };
            break;
        default: break;
    }
    return { s, A::None };   // unexpected input for this state -> ignore
}
```

- [ ] **Step 4: Add the test target** — append to `tests/CMakeLists.txt`

```cmake
add_executable(test_authflow
    test_authflow.cpp
)

target_include_directories(test_authflow PRIVATE
    ${CMAKE_SOURCE_DIR}/src/game/src
)

set_target_properties(test_authflow PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 5: Build + run, verify pass**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_authflow
./out/build/msvc-win64-vs2026-community/bin/Debug/test_authflow.exe
```
Expected: `All authflow tests passed.`

- [ ] **Step 6: Commit**

```bash
git add src/game/src/SessionFlow.h tests/test_authflow.cpp tests/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(net): pure client session FSM (SessionFlow) + unit test"
```

---

## Task 3: AuthServerSystem + WorldServerSystem

**Files:**
- Modify: `src/game/src/game.cpp`

Add the two server systems. They reuse the existing `SendMessage<M>` free function and `WireCodec` dispatch. Hardcoded stub data. Register them now; the demo systems are removed in Task 4 (they coexist harmlessly until then — the servers ignore the demo's Ping opcode).

- [ ] **Step 1: Add flow constants + the two server systems to `game.cpp`** (place near the existing net systems, inside the same anonymous namespace as `SendMessage`)

```cpp
// ---- Dual-server connection flow (prototype; loopback, all in one process) ----
namespace flow {
    constexpr uint16_t kAuthPort  = 27100;
    constexpr uint16_t kWorldPort = 27101;
    constexpr const char* kHost   = "127.0.0.1";
}

// Helper: dispatch on the frame opcode; returns the opcode (0 if runt).
static inline uint16_t FrameOpcode(const NetEvent& ev) {
    return wirecodec::PeekOpcode(ev.payload, ev.len);
}

// AuthServer: login (accept any) + world list + world select (returns world addr + token).
class AuthServerSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const NetServices* net = ctx.Net;
        if (!net) return;
        if (!m_Started) {
            m_Started = true;
            NetServerConfig sc{};
            sc.bind = netlib::Endpoint{ flow::kHost, flow::kAuthPort };
            sc.gameResident = true;
            m_Server = net->CreateServer(&netlib::MakeTcpServer, sc);
            if (m_Server != NetHandle::Invalid) SM_TRACE("AuthServer: listening on %s:%u", flow::kHost, (unsigned)flow::kAuthPort);
            else                                SM_WARN("AuthServer: failed to bind %s:%u", flow::kHost, (unsigned)flow::kAuthPort);
        }
        NetEvent ev{};
        while (net->PollEvent(&ev)) {
            if (ev.adapter != m_Server) continue;
            if (ev.kind != NetEventKind::Message) continue;
            const uint16_t op = FrameOpcode(ev);
            if (op == (uint16_t)wire::OPCODE_LOGIN_REQ) {
                wire::LoginReq req;
                if (!wirecodec::Decode(ev.payload + 2, ev.len - 2, req)) { SM_WARN("AuthServer: bad LoginReq"); continue; }
                wire::LoginResp resp; resp.set_ok(true);          // stub: accept any creds
                SendMessage(net, m_Server, ev.conn, resp);
                SM_TRACE("AuthServer: login '%s' -> ok (conn %llu)", req.username().c_str(), (unsigned long long)ev.conn);
            } else if (op == (uint16_t)wire::OPCODE_WORLD_LIST_REQ) {
                wire::WorldListResp resp;
                auto* w = resp.add_worlds();
                w->set_id(1); w->set_name("Local World"); w->set_host(flow::kHost); w->set_port(flow::kWorldPort);
                SendMessage(net, m_Server, ev.conn, resp);
                SM_TRACE("AuthServer: sent world list (1 world) to conn %llu", (unsigned long long)ev.conn);
            } else if (op == (uint16_t)wire::OPCODE_WORLD_SELECT_REQ) {
                wire::WorldSelectReq req;
                wirecodec::Decode(ev.payload + 2, ev.len - 2, req);
                wire::WorldSelectResp resp;
                resp.set_ok(true);
                resp.set_host(flow::kHost);
                resp.set_port(flow::kWorldPort);
                resp.set_session_token("sess-" + std::to_string(++m_TokenCounter));
                SendMessage(net, m_Server, ev.conn, resp);
                SM_TRACE("AuthServer: world %u selected -> %s:%u token=%s",
                         req.world_id(), flow::kHost, (unsigned)flow::kWorldPort, resp.session_token().c_str());
            }
        }
    }
    const char* Name() const override { return "AuthServerSystem"; }
    SystemPhase Phase() const override { return SystemPhase::PreRender; }
private:
    bool      m_Started = false;
    NetHandle m_Server  = NetHandle::Invalid;
    uint32_t  m_TokenCounter = 0;
};

// WorldServer: session-token handshake (accept any non-empty) + char list + char select + enter-game.
class WorldServerSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const NetServices* net = ctx.Net;
        if (!net) return;
        if (!m_Started) {
            m_Started = true;
            NetServerConfig sc{};
            sc.bind = netlib::Endpoint{ flow::kHost, flow::kWorldPort };
            sc.gameResident = true;
            m_Server = net->CreateServer(&netlib::MakeTcpServer, sc);
            if (m_Server != NetHandle::Invalid) SM_TRACE("WorldServer: listening on %s:%u", flow::kHost, (unsigned)flow::kWorldPort);
            else                                SM_WARN("WorldServer: failed to bind %s:%u", flow::kHost, (unsigned)flow::kWorldPort);
        }
        NetEvent ev{};
        while (net->PollEvent(&ev)) {
            if (ev.adapter != m_Server) continue;
            if (ev.kind != NetEventKind::Message) continue;
            const uint16_t op = FrameOpcode(ev);
            if (op == (uint16_t)wire::OPCODE_SESSION_AUTH_REQ) {
                wire::SessionAuthReq req;
                wirecodec::Decode(ev.payload + 2, ev.len - 2, req);
                wire::SessionAuthResp resp;
                resp.set_ok(!req.session_token().empty());        // stub: any non-empty token
                SendMessage(net, m_Server, ev.conn, resp);
                SM_TRACE("WorldServer: session token '%s' -> %s (conn %llu)",
                         req.session_token().c_str(), resp.ok() ? "ok" : "reject", (unsigned long long)ev.conn);
            } else if (op == (uint16_t)wire::OPCODE_CHAR_LIST_REQ) {
                wire::CharListResp resp;
                auto* c0 = resp.add_chars(); c0->set_id(1); c0->set_name("Hero");  c0->set_level(10);
                auto* c1 = resp.add_chars(); c1->set_id(2); c1->set_name("Rogue"); c1->set_level(7);
                SendMessage(net, m_Server, ev.conn, resp);
                SM_TRACE("WorldServer: sent char list (2) to conn %llu", (unsigned long long)ev.conn);
            } else if (op == (uint16_t)wire::OPCODE_CHAR_SELECT_REQ) {
                wire::CharSelectReq req;
                wirecodec::Decode(ev.payload + 2, ev.len - 2, req);
                wire::CharSelectResp resp; resp.set_ok(true);
                SendMessage(net, m_Server, ev.conn, resp);
                SM_TRACE("WorldServer: char %u selected (conn %llu)", req.char_id(), (unsigned long long)ev.conn);
            } else if (op == (uint16_t)wire::OPCODE_ENTER_GAME_REQ) {
                wire::EnterGameResp resp; resp.set_ok(true);
                SendMessage(net, m_Server, ev.conn, resp);
                SM_TRACE("WorldServer: enter-game ok (conn %llu)", (unsigned long long)ev.conn);
            }
        }
    }
    const char* Name() const override { return "WorldServerSystem"; }
    SystemPhase Phase() const override { return SystemPhase::PreRender; }
private:
    bool      m_Started = false;
    NetHandle m_Server  = NetHandle::Invalid;
};
```

- [ ] **Step 2: Register the two server systems** — in `GameRegisterSystems`, add (next to the existing net systems; leave the demo systems for now):
```cpp
    s->Register(std::make_unique<AuthServerSystem>());
    s->Register(std::make_unique<WorldServerSystem>());
```

- [ ] **Step 3: Build the game, verify clean**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target game
```
Expected: clean build. (Listeners bind at runtime — confirmed in Task 5's smoke; the demo Ping/Pong still runs harmlessly alongside.)

> Engineer note: confirm `#include <string>` is available in `game.cpp` for `std::to_string` (it is — used elsewhere). `wire::*` types come from `wire.pb.h` via `WireCodec.h` (already included). `SendMessage`, `NetServerConfig`, `netlib::Endpoint`, `NetEvent`, `NetEventKind`, `wirecodec::*` are all already in scope (used by the demo systems).

- [ ] **Step 4: Commit**

```bash
git add src/game/src/game.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(net): AuthServerSystem + WorldServerSystem (stub login/world/char handlers)"
```

---

## Task 4: ClientSessionSystem + remove demo systems + state flip

**Files:**
- Modify: `src/game/src/game.cpp`

Add the client that owns both `NetHandle`s and drives `SessionFlow`. Remove the Ping/Pong demo systems and register the three flow systems.

- [ ] **Step 1: Add `#include "SessionFlow.h"`** near the top of `game.cpp` (next to `#include "WireCodec.h"`):
```cpp
#include "SessionFlow.h"
```

- [ ] **Step 2: Add `ClientSessionSystem`** to `game.cpp` (in the same anonymous namespace, after `WorldServerSystem`)

```cpp
// Client: owns BOTH client handles + the session FSM; auto-advances auth -> world -> char -> in-game.
class ClientSessionSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const NetServices* net = ctx.Net;
        if (!net) return;

        // Disconnected: (re)start by connecting the auth client (bounded retry).
        if (m_State == SessionState::Disconnected) {
            m_RetryAccum += ctx.dt;
            const double interval = (m_RetryCount < kMaxFastRetries) ? kFastRetrySec : kSlowRetrySec;
            if (m_RetryAccum < interval) return;
            m_RetryAccum = 0.0;
            CloseClients(net);
            NetClientConfig cc{}; cc.target = netlib::Endpoint{ flow::kHost, flow::kAuthPort }; cc.gameResident = true;
            m_Auth = net->CreateClient(&netlib::MakeTcpClient, cc);
            if (m_RetryCount < kMaxFastRetries) m_RetryCount++;
            if (m_Auth == NetHandle::Invalid) { SM_WARN("ClientSession: auth CreateClient failed"); return; }
            m_State = SessionState::ConnectingAuth;
            SM_TRACE("ClientSession: connecting to auth %s:%u", flow::kHost, (unsigned)flow::kAuthPort);
            return;
        }

        NetEvent ev{};
        while (net->PollEvent(&ev)) {
            const bool isAuth  = (ev.adapter == m_Auth);
            const bool isWorld = (ev.adapter == m_World);
            if (!isAuth && !isWorld) continue;

            if (ev.kind == NetEventKind::Connected) {
                Feed(ctx, net, isAuth ? SessionInput::AuthConnected : SessionInput::WorldConnected);
            } else if (ev.kind == NetEventKind::Disconnected || ev.kind == NetEventKind::Error) {
                Feed(ctx, net, SessionInput::Dropped);
            } else if (ev.kind == NetEventKind::Message) {
                Feed(ctx, net, ClassifyReply(ev));
            }
        }
    }
    const char* Name() const override { return "ClientSessionSystem"; }
    SystemPhase Phase() const override { return SystemPhase::PreRender; }

private:
    // Map a received reply frame to a SessionInput (decoding ok-flags + stashing flow data).
    SessionInput ClassifyReply(const NetEvent& ev) {
        const uint16_t op = FrameOpcode(ev);
        const uint8_t* body = ev.payload + 2; const uint32_t blen = ev.len - 2;
        switch (op) {
            case (uint16_t)wire::OPCODE_LOGIN_RESP: {
                wire::LoginResp r; return (wirecodec::Decode(body, blen, r) && r.ok()) ? SessionInput::LoginOk : SessionInput::LoginFail;
            }
            case (uint16_t)wire::OPCODE_WORLD_LIST_RESP: {
                wire::WorldListResp r;
                if (wirecodec::Decode(body, blen, r) && r.worlds_size() > 0) { m_PickWorldId = r.worlds(0).id(); return SessionInput::WorldListReceived; }
                return SessionInput::Dropped;
            }
            case (uint16_t)wire::OPCODE_WORLD_SELECT_RESP: {
                wire::WorldSelectResp r;
                if (wirecodec::Decode(body, blen, r) && r.ok()) {
                    m_WorldHost = r.host(); m_WorldPort = (uint16_t)r.port(); m_Token = r.session_token();
                    return SessionInput::WorldSelectOk;
                }
                return SessionInput::WorldSelectFail;
            }
            case (uint16_t)wire::OPCODE_SESSION_AUTH_RESP: {
                wire::SessionAuthResp r; return (wirecodec::Decode(body, blen, r) && r.ok()) ? SessionInput::SessionAuthOk : SessionInput::SessionAuthFail;
            }
            case (uint16_t)wire::OPCODE_CHAR_LIST_RESP: {
                wire::CharListResp r;
                if (wirecodec::Decode(body, blen, r) && r.chars_size() > 0) { m_PickCharId = r.chars(0).id(); return SessionInput::CharListReceived; }
                return SessionInput::Dropped;
            }
            case (uint16_t)wire::OPCODE_CHAR_SELECT_RESP: {
                wire::CharSelectResp r; return (wirecodec::Decode(body, blen, r) && r.ok()) ? SessionInput::CharSelectOk : SessionInput::CharSelectFail;
            }
            case (uint16_t)wire::OPCODE_ENTER_GAME_RESP: {
                wire::EnterGameResp r; return (wirecodec::Decode(body, blen, r) && r.ok()) ? SessionInput::EnterGameOk : SessionInput::EnterGameFail;
            }
            default: return SessionInput::Dropped;   // unexpected opcode -> treat as a failure
        }
    }

    void Feed(SystemContext& ctx, const NetServices* net, SessionInput in) {
        SessionStep step = AdvanceSession(m_State, in);
        if (step.next != m_State)
            SM_TRACE("ClientSession: %d -(%d)-> %d", (int)m_State, (int)in, (int)step.next);
        m_State = step.next;
        DoAction(ctx, net, step.action);
    }

    void DoAction(SystemContext& ctx, const NetServices* net, SessionAction a) {
        switch (a) {
            case SessionAction::SendLogin: {
                wire::LoginReq r; r.set_username("player"); r.set_password("stub");
                SendMessage(net, m_Auth, kNetConnInvalid, r); break;
            }
            case SessionAction::SendWorldListReq: { wire::WorldListReq r; SendMessage(net, m_Auth, kNetConnInvalid, r); break; }
            case SessionAction::SendWorldSelect:  { wire::WorldSelectReq r; r.set_world_id(m_PickWorldId); SendMessage(net, m_Auth, kNetConnInvalid, r); break; }
            case SessionAction::BeginHandoff: {
                if (m_Auth != NetHandle::Invalid) { net->Close(m_Auth); m_Auth = NetHandle::Invalid; }
                NetClientConfig cc{}; cc.target = netlib::Endpoint{ m_WorldHost, m_WorldPort }; cc.gameResident = true;
                m_World = net->CreateClient(&netlib::MakeTcpClient, cc);
                SM_TRACE("ClientSession: handoff -> world %s:%u", m_WorldHost.c_str(), (unsigned)m_WorldPort);
                if (m_World == NetHandle::Invalid) Feed(ctx, net, SessionInput::Dropped);
                break;
            }
            case SessionAction::SendSessionAuth: { wire::SessionAuthReq r; r.set_session_token(m_Token); SendMessage(net, m_World, kNetConnInvalid, r); break; }
            case SessionAction::SendCharListReq: { wire::CharListReq r; SendMessage(net, m_World, kNetConnInvalid, r); break; }
            case SessionAction::SendCharSelect:  { wire::CharSelectReq r; r.set_char_id(m_PickCharId); SendMessage(net, m_World, kNetConnInvalid, r); break; }
            case SessionAction::SendEnterGame:   { wire::EnterGameReq r; SendMessage(net, m_World, kNetConnInvalid, r); break; }
            case SessionAction::EnterGame:
                ctx.world.ModifySingleton<GameStateComponent>([](GameStateComponent& g){ g.Current = GameStateId::InLevel; });
                SM_TRACE("ClientSession: IN GAME — flow complete; GameState -> InLevel");
                break;
            case SessionAction::Reset:
                SM_WARN("ClientSession: flow reset; will retry");
                CloseClients(net);
                m_State = SessionState::Disconnected;
                m_RetryAccum = 0.0;
                break;
            case SessionAction::None: default: break;
        }
    }

    void CloseClients(const NetServices* net) {
        if (m_Auth  != NetHandle::Invalid) { net->Close(m_Auth);  m_Auth  = NetHandle::Invalid; }
        if (m_World != NetHandle::Invalid) { net->Close(m_World); m_World = NetHandle::Invalid; }
    }

    static constexpr int    kMaxFastRetries = 20;
    static constexpr double kFastRetrySec   = 0.5;
    static constexpr double kSlowRetrySec   = 5.0;

    SessionState m_State = SessionState::Disconnected;
    NetHandle    m_Auth  = NetHandle::Invalid;
    NetHandle    m_World = NetHandle::Invalid;
    uint32_t     m_PickWorldId = 0;
    uint32_t     m_PickCharId  = 0;
    std::string  m_WorldHost;
    uint16_t     m_WorldPort   = 0;
    std::string  m_Token;
    int          m_RetryCount  = 0;
    double       m_RetryAccum  = 0.0;
};
```

- [ ] **Step 3: Remove the demo systems + update registration.** Delete the `NetServerSystem` and `NetClientSystem` class definitions (the Ping/Pong demo) from `game.cpp`. In `GameRegisterSystems`, remove the two demo registrations:
```cpp
    s->Register(std::make_unique<NetServerSystem>());   // DELETE
    s->Register(std::make_unique<NetClientSystem>());   // DELETE
```
and ensure the three flow systems are registered (AuthServer + WorldServer from Task 3, plus the client):
```cpp
    s->Register(std::make_unique<AuthServerSystem>());
    s->Register(std::make_unique<WorldServerSystem>());
    s->Register(std::make_unique<ClientSessionSystem>());
```

> Engineer note: the demo systems' Ping/Pong `SendMessage` calls and the `wire::Ping/Pong` round-trip in the boot smoke can stay (Ping/Pong messages still exist in the schema). Only the two demo *systems* are removed. If `m_Seq`/Ping heartbeat is wanted in-game later, it's a follow-up; not in this task.

- [ ] **Step 4: Build game + editor, verify clean**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target game
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: both clean.

- [ ] **Step 5: Manual smoke (USER-owned — interactive)**

Launch `editor.exe`. In the log, confirm the flow walks end to end:
```
AuthServer: listening on 127.0.0.1:27100
WorldServer: listening on 127.0.0.1:27101
ClientSession: connecting to auth 127.0.0.1:27100
ClientSession: 1 -(0)-> 2        (ConnectingAuth -> Authenticating)
AuthServer: login 'player' -> ok
ClientSession: ... WorldSelecting ...
AuthServer: sent world list / world 1 selected -> 127.0.0.1:27101 token=sess-1
ClientSession: handoff -> world 127.0.0.1:27101
WorldServer: session token 'sess-1' -> ok
WorldServer: sent char list / char 1 selected / enter-game ok
ClientSession: IN GAME — flow complete; GameState -> InLevel
```
**USER ACTION:** accept any Windows Firewall prompt on first listen/connect (loopback usually skips it).

- [ ] **Step 6: Commit**

```bash
git add src/game/src/game.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(net): ClientSessionSystem drives auth->world->char->in-game; drop Ping/Pong demo"
```

---

## Task 5: Full build, regression, manual smoke, docs

**Files:**
- Modify: `docs/superpowers/specs/2026-05-31-dual-server-connection-flow-design.md` (status)

- [ ] **Step 1: Full rebuild + run the relevant test suite**

Run:
```
cmake --build --preset msvc-win64-vs2026-community
./out/build/msvc-win64-vs2026-community/bin/Debug/test_authflow.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_net.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_netlib.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: all green (`All authflow tests passed.`, `All net tests passed.`, `All netlib tests passed.`, `All ECS tests passed.`). Full all-targets build succeeds.

- [ ] **Step 2: Manual smoke confirmation** (same as Task 4 Step 5) — confirm the editor walks the full flow to `IN GAME` and `GameState -> InLevel`, both listeners bound, handoff to `27101`, and `flow-proto round-trip OK` logged at boot.

- [ ] **Step 3: Mark the spec done**

In `docs/superpowers/specs/2026-05-31-dual-server-connection-flow-design.md`, change `Status:` to implemented + a one-line verification note (authflow unit test green; manual smoke walks to in-game).

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-05-31-dual-server-connection-flow-design.md
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "docs(net): mark dual-server connection flow implemented"
```

---

## Self-Review

**Spec coverage:**
- AuthServer (login + world list + world select w/ addr+token) → Task 3 `AuthServerSystem`. ✓
- WorldServer (session auth + char list + char select + enter-game) → Task 3 `WorldServerSystem`. ✓
- Two client handles + session FSM + handoff → Task 4 `ClientSessionSystem` (owns `m_Auth`/`m_World`) + Task 2 `SessionFlow`. ✓
- State machine `auth→world-select→handoff→char-select→in-game` → Task 2 (pure) + Task 4 (driven). ✓
- Protocol messages/opcodes + WireCodec → Task 1. ✓
- All in one process, no role gating, loopback two ports → Tasks 3/4 (systems run unconditionally; `kAuthPort`/`kWorldPort`). ✓
- Auto-advance + stub data (accept-any auth, hardcoded worlds/chars, `sess-<counter>` token) → Tasks 3/4. ✓
- Flip `GameStateComponent`→`InLevel` on in-game → Task 4 `SessionAction::EnterGame`. ✓
- Replace Ping/Pong demo systems → Task 4 Step 3. ✓
- Tests: pure FSM unit test (Task 2), boot-smoke proto round-trip (Task 1), manual e2e smoke (Tasks 4/5). ✓
- Bounded retry → Task 4 (`kMaxFastRetries`/fast+slow). ✓

**Placeholder scan:** no TBD/TODO; every code step is complete; engineer-notes only ask to confirm existing includes/scope, not defer design.

**Type consistency:** `SessionState`/`SessionInput`/`SessionAction`/`SessionStep`/`AdvanceSession` identical across `SessionFlow.h` (Task 2), the test (Task 2), and `ClientSessionSystem` (Task 4). `flow::kAuthPort`/`kWorldPort`/`kHost` consistent across Tasks 3/4. `FrameOpcode` defined in Task 3, used in Task 4 (both in `game.cpp`, same TU). `SendMessage`/`wirecodec::*`/`wire::*` names match the as-built helpers. Opcode names match between `wire.proto` (Task 1), `WireCodec.h` specializations (Task 1), and the server/client dispatch (Tasks 3/4). `ModifySingleton<GameStateComponent>` + `GameStateId::InLevel` match the engine API (used by `AppFlowSystem::SetState`).

**Note:** `FrameOpcode` is introduced in Task 3 and reused in Task 4 — both edit the same `game.cpp` anonymous namespace, so it's defined once (Task 3) and in scope for Task 4. If Tasks are executed out of order, Task 4 depends on Task 3's `FrameOpcode` + `flow::` constants + the two server systems being present.
