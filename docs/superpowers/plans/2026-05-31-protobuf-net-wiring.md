# Protobuf Net-Wiring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire real `wire::Ping`/`wire::Pong` protobuf messages through the demo `NetClientSystem`/`NetServerSystem` over the agnostic zero-copy transport, serialized in place into the pooled send buffer.

**Architecture:** Add two pure helpers to `WireCodec` (`EncodeInto` = serialize `[u16 opcode][protobuf]` straight into a caller buffer; `PeekOpcode`). A game-side `SendMessage<M>` does `AcquireSend → EncodeInto → Send` (no extra copy). The demo systems swap their raw `[u16 tag]["ping"]` for protobuf Ping/Pong with opcode dispatch. Game-side only — no engine/transport change.

**Tech Stack:** C++23, Google protobuf-lite (`wire.pb`, already linked into `Game.dll`), the agnostic `NetServices` API, CMake/MSVC (msvc-win64-vs2026-enterprise).

**Build/verify commands:**
- Build game: `cmake --build out/build/msvc-win64-vs2026-enterprise --target game`
- Build editor: `cmake --build out/build/msvc-win64-vs2026-enterprise --target editor`

**Reference:** spec `docs/superpowers/specs/2026-05-31-protobuf-net-wiring-design.md`.

> **Note on testing:** the `game` target has no unit-test harness. The executable self-check is the boot smoke block in `game.cpp` (the `Uninitialized` branch), extended in Task 1 to round-trip via the new helpers. It logs `SM_TRACE` on success / `SM_ERROR` on failure (game has no `platform_debug_break`, so no `SM_ASSERT`). A clean `game` build is the compile-time test of the templates; the runtime Ping/Pong exchange is a manual GUI/socket check noted at the end.

---

### Task 1: `WireCodec::EncodeInto` + `PeekOpcode` (+ extend the boot self-check)

**Files:**
- Modify: `src/game/src/WireCodec.h` (add `EncodeInto`, `PeekOpcode`)
- Modify: `src/game/src/game.cpp` (extend the boot smoke block to exercise them)

- [ ] **Step 1: Add the two helpers to WireCodec.h**

In `src/game/src/WireCodec.h`, inside `namespace wirecodec {`, after the existing `Decode` template and before `OpcodeOf` (order doesn't matter, but `EncodeInto` calls `OpcodeOf`, which is declared below — so place `EncodeInto`/`PeekOpcode` AFTER the `OpcodeOf` specializations, just before the closing `}` of the namespace). Add:
```cpp
// Serialize [u16 opcode][protobuf] for `msg` straight into `dst` (cap bytes).
// opcode = OpcodeOf<M>(). Returns total bytes written (2 + ByteSize), or 0 if cap
// is too small. The caller sizes dst via AcquireSend(2 + msg.ByteSizeLong()).
template <class M>
uint32_t EncodeInto(uint8_t* dst, uint32_t cap, const M& msg) {
    const uint32_t n = static_cast<uint32_t>(msg.ByteSizeLong());
    if (cap < 2u + n) return 0;
    const uint16_t op = static_cast<uint16_t>(OpcodeOf<M>());
    dst[0] = static_cast<uint8_t>(op & 0xff);
    dst[1] = static_cast<uint8_t>((op >> 8) & 0xff);
    if (n) (void)msg.SerializeToArray(dst + 2, static_cast<int>(n));
    return 2u + n;
}

// Read the leading [u16 opcode] from a received frame (0 if len < 2).
inline uint16_t PeekOpcode(const uint8_t* frame, uint32_t len) {
    return len >= 2 ? static_cast<uint16_t>(frame[0]) |
                      (static_cast<uint16_t>(frame[1]) << 8)
                    : 0;
}
```
`<cstdint>` is already included in `WireCodec.h`. `EncodeInto` is a template (instantiated at the call site in Task 2), so it must appear after the `OpcodeOf<M>()` specializations it calls.

- [ ] **Step 2: Replace the boot smoke block with the new-path round-trip**

In `src/game/src/game.cpp`, find the existing smoke-test block inside the `if (g_GameState->StateId == GameStateId::Uninitialized) {` boot branch (the block that builds a `wire::Ping`, calls `wirecodec::Encode`/`Decode`, and logs `protobuf smoke test OK`). Replace that entire `{ ... }` block with:
```cpp
        // One-time protobuf wire-format self-check: exercise EncodeInto (serialize
        // [u16 opcode][protobuf] in place) + PeekOpcode + Decode round-trip. NOT wired
        // into the live net systems here — this is plumbing validation only. Logs
        // (not SM_ASSERT): the game target has no platform_debug_break to link SM_ASSERT.
        {
            wire::Ping ping;
            ping.set_seq(7);
            ping.set_client_time_ms(999);

            uint8_t buf[64];
            const uint32_t n = wirecodec::EncodeInto(buf, sizeof(buf), ping);
            const bool okOp = n >= 2 && wirecodec::PeekOpcode(buf, n) ==
                              static_cast<uint16_t>(wire::OPCODE_PING);
            wire::Ping back;
            const bool okBody = n >= 2 && wirecodec::Decode(buf + 2, n - 2, back);
            if (okOp && okBody && back.seq() == 7u && back.client_time_ms() == 999ull)
                SM_TRACE("[GAMEDLL] protobuf EncodeInto/PeekOpcode/Decode round-trip OK (seq=%u, %u bytes)",
                         back.seq(), n);
            else
                SM_ERROR("[GAMEDLL] protobuf wire round-trip FAILED");
        }
```

- [ ] **Step 3: Build game — compile-time test of the templates**

Run:
```
cmake --build out/build/msvc-win64-vs2026-enterprise --target game
```
Expected: `Game.dll` builds clean (the new `EncodeInto`/`PeekOpcode` compile and instantiate for `wire::Ping`; the smoke block compiles).

- [ ] **Step 4: Commit**
```
git add src/game/src/WireCodec.h src/game/src/game.cpp
git commit -m "feat(net): WireCodec EncodeInto (serialize-in-place) + PeekOpcode"
```

---

### Task 2: Port the demo net systems to protobuf Ping/Pong

**Files:**
- Modify: `src/game/src/game.cpp` (replace `SendTagged`/`FrameTag` glue; port `NetServerSystem`/`NetClientSystem`)

- [ ] **Step 1: Replace the raw-tag glue with `SendMessage<M>`**

In `src/game/src/game.cpp`, find the file-local helper block (the anonymous `namespace { ... }` just above `NetServerSystem` containing `kTagPing`/`kTagPong`, `SendTagged`, `FrameTag`). Replace that entire `namespace { ... }` block with:
```cpp
namespace {
// Serialize a protobuf message into a pooled send buffer ([u16 opcode][protobuf])
// and send it — zero extra copy (serialize-in-place via wirecodec::EncodeInto).
template <class M>
bool SendMessage(const NetServices* net, NetHandle h, NetConnId conn, const M& msg) {
    const uint32_t total = 2u + static_cast<uint32_t>(msg.ByteSizeLong());
    SendBuffer sb = net->AcquireSend(total);
    if (!sb.data) return false;
    const uint32_t written = wirecodec::EncodeInto(sb.data, sb.cap, msg);
    if (written == 0) { net->AbortSend(sb); return false; }   // cap == total, so this shouldn't happen
    return net->Send(h, conn, sb, written);
}
} // namespace
```
(`WireCodec.h` is already `#include`d in `game.cpp`; `NetServices`/`SendBuffer`/`NetHandle`/`NetConnId` are available via the engine headers the file already uses for the net systems.)

- [ ] **Step 2: Port `NetServerSystem` — decode Ping, reply Pong**

In `NetServerSystem::Update`, replace the `PollEvent` loop body's message branch. The current branch is:
```cpp
            if (ev.kind == NetEventKind::Message && FrameTag(ev) == kTagPing) {
                const uint8_t* body = ev.payload + 2; const uint32_t bodyLen = ev.len - 2;
                SendTagged(net, m_Server, ev.conn, kTagPong, body, bodyLen);   // echo as pong
                SM_TRACE("NetDemo[server]: echoed ping from conn %llu", (unsigned long long)ev.conn);
            } else if (ev.kind == NetEventKind::Connected) {
```
Replace it (keep the `Connected` branch and the surrounding loop) with:
```cpp
            if (ev.kind == NetEventKind::Message &&
                wirecodec::PeekOpcode(ev.payload, ev.len) == static_cast<uint16_t>(wire::OPCODE_PING)) {
                wire::Ping ping;
                if (wirecodec::Decode(ev.payload + 2, ev.len - 2, ping)) {
                    wire::Pong pong;
                    pong.set_seq(ping.seq());
                    pong.set_server_time_ms(static_cast<uint64_t>(ctx.gameTime * 1000.0));
                    SendMessage(net, m_Server, ev.conn, pong);
                    SM_TRACE("NetDemo[server]: echoed Ping seq=%u -> Pong (conn %llu)",
                             ping.seq(), (unsigned long long)ev.conn);
                } else {
                    SM_WARN("NetDemo[server]: malformed Ping dropped");
                }
            } else if (ev.kind == NetEventKind::Connected) {
```
(The `SystemContext& ctx` parameter is already in scope in `Update`.)

- [ ] **Step 3: Port `NetClientSystem` — send Ping, decode Pong**

Add a sequence-counter member to `NetClientSystem` (in its `private:` section, next to `m_PingAccum` etc.):
```cpp
    uint32_t m_Seq = 0;
```
Replace the client's receive `Pong` branch. Current:
```cpp
            } else if (ev.kind == NetEventKind::Message && FrameTag(ev) == kTagPong) {
                SM_TRACE("NetDemo[client]: got echo (%u bytes)", ev.len);
            } else if (ev.kind == NetEventKind::Disconnected || ev.kind == NetEventKind::Error) {
```
Replace with:
```cpp
            } else if (ev.kind == NetEventKind::Message &&
                       wirecodec::PeekOpcode(ev.payload, ev.len) == static_cast<uint16_t>(wire::OPCODE_PONG)) {
                wire::Pong pong;
                if (wirecodec::Decode(ev.payload + 2, ev.len - 2, pong))
                    SM_TRACE("NetDemo[client]: got Pong seq=%u server_time_ms=%llu",
                             pong.seq(), (unsigned long long)pong.server_time_ms());
                else
                    SM_WARN("NetDemo[client]: malformed Pong dropped");
            } else if (ev.kind == NetEventKind::Disconnected || ev.kind == NetEventKind::Error) {
```
Replace the ping-send block. Current:
```cpp
        if (m_Connected) {
            m_PingAccum += ctx.dt;
            if (m_PingAccum >= kPingIntervalSec) {
                m_PingAccum = 0.0;
                const uint8_t payload[4] = { 'p','i','n','g' };
                SendTagged(net, m_Client, kNetConnInvalid, kTagPing, payload, sizeof(payload));
                SM_TRACE("NetDemo[client]: sent ping (tag 1)");
            }
        }
```
Replace with:
```cpp
        if (m_Connected) {
            m_PingAccum += ctx.dt;
            if (m_PingAccum >= kPingIntervalSec) {
                m_PingAccum = 0.0;
                wire::Ping ping;
                ping.set_seq(++m_Seq);
                ping.set_client_time_ms(static_cast<uint64_t>(ctx.gameTime * 1000.0));
                SendMessage(net, m_Client, kNetConnInvalid, ping);
                SM_TRACE("NetDemo[client]: sent Ping seq=%u", ping.seq());
            }
        }
```

- [ ] **Step 4: Update the NetServerSystem/NetClientSystem header comment**

The block comment above `NetServerSystem` (starting "NetServerSystem / NetClientSystem — minimal loopback networking demo") still says "The client pings (opcode 1); the server echoes (opcode 2)". Update that sentence to reflect protobuf:
```cpp
// client connects to it and pings every 2s. Both go through the engine NetServices
// bridge (ctx.Net). The client sends a wire::Ping (opcode OPCODE_PING); the server
// decodes it and replies wire::Pong (OPCODE_PONG); both log the seq round-trip.
```
(Adjust the exact surrounding lines to keep the comment coherent — replace only the stale "pings (opcode 1)/echoes (opcode 2)" description.)

- [ ] **Step 5: Build game + editor**

Run:
```
cmake --build out/build/msvc-win64-vs2026-enterprise --target game
cmake --build out/build/msvc-win64-vs2026-enterprise --target editor
```
Expected: both build clean. `game.cpp` compiles with `SendMessage<wire::Ping>`/`SendMessage<wire::Pong>` instantiated; no references to the removed `kTagPing`/`kTagPong`/`SendTagged`/`FrameTag` remain (grep `SendTagged`/`FrameTag`/`kTagPing` in `game.cpp` → no hits).

- [ ] **Step 6: Commit**
```
git add src/game/src/game.cpp
git commit -m "feat(net): wire protobuf Ping/Pong through the demo net systems"
```

---

## Manual verification (GUI/sockets — cannot run headless)

After Task 2, to confirm the live exchange:
1. Build `server` (`cmake --build out/build/msvc-win64-vs2026-enterprise --target server`) and run `server.exe` (the dedicated server boots `Game.dll` as `AppRole::Server`).
2. Run `editor.exe` (the client).
3. Confirm the console shows, repeating ~every 2s:
   - client: `NetDemo[client]: sent Ping seq=N`
   - server: `NetDemo[server]: echoed Ping seq=N -> Pong`
   - client: `NetDemo[client]: got Pong seq=N server_time_ms=…`
   with `seq` matching across the three lines.
4. At editor boot, also confirm the one-time `[GAMEDLL] protobuf EncodeInto/PeekOpcode/Decode round-trip OK (seq=7, …)` line (and no `FAILED`).

---

## Self-Review

**Spec coverage:**
- `WireCodec::EncodeInto` (serialize-in-place) + `PeekOpcode` → Task 1.
- `SendMessage<M>` glue (AcquireSend→EncodeInto→Send, AbortSend on failure) → Task 2 Step 1.
- Client sends `wire::Ping{seq, client_time_ms}`, logs sent seq → Task 2 Step 3.
- Server decodes Ping, replies `wire::Pong{seq, server_time_ms}` → Task 2 Step 2.
- Client decodes Pong, logs seq round-trip → Task 2 Step 3.
- opcode = `OpcodeOf<M>()`/`wire::OPCODE_*`; remove raw `kTagPing/kTagPong` → Task 2 Steps 1–3.
- Timestamp `ctx.gameTime*1000` → Task 2 Steps 2–3.
- Error handling (PeekOpcode 0 / Decode false / AcquireSend fail) → Task 2 Steps 1–3 + spec §6.
- Boot smoke block extended as the self-check → Task 1 Step 2.
- Hot-reload safe (.cpp-only, per-tick scratch messages) → respected (no `Game.h`/CMake change).
- Out of scope (Snapshot, engine change) → respected.

**Placeholder scan:** none — every step has concrete code/commands.

**Type consistency:** `EncodeInto(uint8_t*, uint32_t, const M&) -> uint32_t`, `PeekOpcode(const uint8_t*, uint32_t) -> uint16_t`, `SendMessage(const NetServices*, NetHandle, NetConnId, const M&) -> bool`, `wirecodec::Decode(const uint8_t*, uint32_t, M&)`, `wire::OPCODE_PING/PONG`, `m_Seq` — consistent across Tasks 1–2 and the spec. `OPCODE_*` enum cast to `uint16_t` consistently for comparison with `PeekOpcode`.

**Ordering note (verified):** `EncodeInto` calls `OpcodeOf<M>()`, so it is placed AFTER the `OpcodeOf` specializations in `WireCodec.h` (Task 1 Step 1) — a template instantiated only at the Task 2 call sites, so the ordering compiles.
