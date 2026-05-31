# Protobuf wire-format into the net systems — design

**Date:** 2026-05-31
**Status:** Approved (brainstorming), pending implementation plan
**Scope:** Wire real `wire::Ping`/`wire::Pong` protobuf messages through the demo `NetClientSystem`/`NetServerSystem` over the agnostic zero-copy transport. The deferred follow-up to the protobuf wire-format plumbing + the transport reshape.

## Goal

The transport is now a serialization-agnostic `[u32 len][opaque]` pipe, and `WireCodec`/`wire.proto` provide protobuf-lite messages. The demo net systems currently send a raw `[u16 tag]["ping"]`. Replace that with `[u16 opcode][protobuf]`, serialized **directly into the pooled send buffer** (no extra copy), with opcode-based dispatch on receive — proving the full path end to end.

## Context (as-built)

- **Transport:** `NetServices::AcquireSend(n)` hands a writable `SendBuffer` (pool block ≤16 KB / heap), the caller serializes in place, `Send(h, conn, sb, len)` moves it down; `PollEvent` yields an opaque `NetEvent{conn, payload, len}` (no opcode field). The engine knows nothing about message types.
- **WireCodec** (`src/game/src/WireCodec.h`, header-only, game-local): `Encode<M>` (vector), `Decode<M>(data,len,out)` (`ParseFromArray`), `OpcodeOf<M>()` (specialized for `Ping`/`Pong`/`Snapshot`). Depends only on `wire.pb.h`.
- **Schema** (`src/game/proto/wire.proto`): `Ping{uint32 seq; uint64 client_time_ms}`, `Pong{uint32 seq; uint64 server_time_ms}`, plus `Vec3`/`PlayerState`/`Snapshot` (defined, unused here). `enum Opcode { UNSPECIFIED=0, PING=1, PONG=2, SNAPSHOT=3 }`.
- **Net systems** (`src/game/src/game.cpp`): `NetServerSystem` (echoes) / `NetClientSystem` (connect/retry/2s ping) use file-local `SendTagged`/`FrameTag` over a raw `[u16 tag]`. `game.cpp` already `#include "WireCodec.h"` and links protobuf-lite + the generated `wire.pb` — no build change needed.
- **Wire layout (unchanged):** `[u32 len]` (engine framing) + game payload `[u16 opcode][protobuf message bytes]`. The opcode lives inside the opaque frame; the engine never sees it.

## Design

### 1. WireCodec — two pure helpers (no `NetServices` dependency)

Add to `WireCodec.h` (keeps it reusable by future real systems; the AcquireSend/Send glue stays in the net systems):

```cpp
// Serialize [u16 opcode][protobuf] for `msg` straight into `dst` (cap bytes).
// opcode = OpcodeOf<M>(). Returns the total bytes written (2 + ByteSize), or 0 if
// cap is too small. The caller sizes dst via AcquireSend(2 + msg.ByteSizeLong()).
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
Keep `Encode`/`Decode`/`OpcodeOf` unchanged. `Decode` is used on the body (`frame + 2`, `len - 2`).

### 2. game.cpp net glue — replace `SendTagged`/`FrameTag`

```cpp
// Serialize a protobuf message into a pooled send buffer ([u16 opcode][protobuf])
// and send it — zero extra copy (serialize-in-place via EncodeInto).
template <class M>
bool SendMessage(const NetServices* net, NetHandle h, NetConnId conn, const M& msg) {
    const uint32_t total = 2u + static_cast<uint32_t>(msg.ByteSizeLong());
    SendBuffer sb = net->AcquireSend(total);
    if (!sb.data) return false;
    const uint32_t written = wirecodec::EncodeInto(sb.data, sb.cap, msg);
    if (written == 0) { net->AbortSend(sb); return false; }   // shouldn't happen (cap == total)
    return net->Send(h, conn, sb, written);
}
```
Receive dispatch uses `wirecodec::PeekOpcode(ev.payload, ev.len)` then `wirecodec::Decode<M>(ev.payload + 2, ev.len - 2, msg)`. Remove the raw `kTagPing`/`kTagPong` constants and the `SendTagged`/`FrameTag` helpers.

### 3. Behavior

- **`NetClientSystem`** — connect/retry/2s cadence unchanged. Add a `uint32_t m_Seq = 0` member. On each ping tick:
  ```cpp
  wire::Ping ping;
  ping.set_seq(++m_Seq);
  ping.set_client_time_ms(static_cast<uint64_t>(ctx.gameTime * 1000.0));
  SendMessage(net, m_Client, kNetConnInvalid, ping);
  SM_TRACE("NetDemo[client]: sent Ping seq=%u", ping.seq());
  ```
  On receive, when `PeekOpcode(ev.payload, ev.len) == wire::OPCODE_PONG`:
  ```cpp
  wire::Pong pong;
  if (wirecodec::Decode(ev.payload + 2, ev.len - 2, pong))
      SM_TRACE("NetDemo[client]: got Pong seq=%u server_time_ms=%llu",
               pong.seq(), (unsigned long long)pong.server_time_ms());
  else
      SM_WARN("NetDemo[client]: malformed Pong dropped");
  ```
- **`NetServerSystem`** — on receive, when `PeekOpcode(...) == wire::OPCODE_PING`:
  ```cpp
  wire::Ping ping;
  if (wirecodec::Decode(ev.payload + 2, ev.len - 2, ping)) {
      wire::Pong pong;
      pong.set_seq(ping.seq());
      pong.set_server_time_ms(static_cast<uint64_t>(ctx.gameTime * 1000.0));
      SendMessage(net, m_Server, ev.conn, pong);
      SM_TRACE("NetDemo[server]: echoed Ping seq=%u -> Pong", ping.seq());
  } else {
      SM_WARN("NetDemo[server]: malformed Ping dropped");
  }
  ```
  Keep the `Connected`/listener logic.

### 4. Timestamps

`client_time_ms` / `server_time_ms` = `static_cast<uint64_t>(ctx.gameTime * 1000.0)` (`SystemContext::gameTime`, seconds since boot — already used by `DayNightSystem`). No wall clock; sufficient for the demo. (`SystemContext::dt`/`gameTime` confirmed available — the systems already read `ctx.dt`.)

### 5. Hot-reload

protobuf message objects are per-tick locals (scratch) → reload-safe. `m_Seq` is a per-system member; it resets to 0 on a Game.dll reload (acceptable — a demo counter). protobuf-lite + `wire.pb` already link into `Game.dll`; this is a `.cpp`-only change (no `Game.h` / `GAME_API_VERSION` change), so it hot-reloads live.

### 6. Error handling

- `PeekOpcode` returns 0 for `len < 2` → no dispatch (runt frame ignored).
- `Decode` (`ParseFromArray`) false on malformed/truncated → log `SM_WARN` + skip; never crash.
- `SendMessage`: `AcquireSend` fail (pool + heap both fail — effectively never) → return false, skip; `EncodeInto` 0 (cap mismatch — shouldn't happen since `cap == total`) → `AbortSend` + false.
- Unknown opcode (e.g. server gets a Pong, client gets a Ping) → ignored (no matching branch).

### 7. Testing

The `game` target has no unit harness. The executable self-check is the existing boot smoke block (`game.cpp`, `Uninitialized` branch) — extend it to exercise the new path in place of (or alongside) the current vector round-trip:
```cpp
wire::Ping ping; ping.set_seq(7); ping.set_client_time_ms(999);
uint8_t buf[64];
const uint32_t n = wirecodec::EncodeInto(buf, sizeof(buf), ping);
const bool okOp = n >= 2 && wirecodec::PeekOpcode(buf, n) == wire::OPCODE_PING;
wire::Ping back;
const bool okBody = n >= 2 && wirecodec::Decode(buf + 2, n - 2, back);
if (okOp && okBody && back.seq() == 7 && back.client_time_ms() == 999)
    SM_TRACE("[GAMEDLL] protobuf EncodeInto/PeekOpcode/Decode round-trip OK (seq=%u)", back.seq());
else
    SM_ERROR("[GAMEDLL] protobuf wire round-trip FAILED");
```
Live verification (manual, GUI/sockets): run a server + the editor client; confirm the console shows `client: sent Ping seq=N`, `server: echoed Ping seq=N -> Pong`, `client: got Pong seq=N server_time_ms=…` with `seq` matching round-trip.

## Out of scope

- `Snapshot`/`PlayerState`/server-initiated messages (defined, unused — future).
- Authoritative simulation / real game state over the wire.
- Any transport/engine change — the agnostic API and pools are done; this is game-side only.

## Decisions locked

- `[u16 opcode][protobuf]` inside the opaque frame; opcode = `OpcodeOf<M>()` (`wire::OPCODE_*`).
- Serialize-in-place via `wirecodec::EncodeInto` into the `AcquireSend` block — no extra copy.
- Ping/Pong seq + `gameTime*1000` timestamp round-trip; client logs the round-trip seq.
- WireCodec stays `NetServices`-free; the AcquireSend/Send glue (`SendMessage`) lives in game.cpp.
- `.cpp`-only change (hot-reloadable); boot smoke block extended as the self-check.
