# Agnostic zero-copy net transport reshape — design

**Date:** 2026-05-31
**Status:** Approved (brainstorming), pending implementation plan
**Scope:** Reshape the engine + netlib networking transport to (a) be fully serialization-agnostic and (b) remove the unnecessary user-space copies in the send/recv paths. **No game wiring** and **no protobuf** in this work — that is a separate follow-up.

## Goal

Today the networking stack triple-copies payloads on each side beyond the mandatory serialize/parse + kernel copy, and it bakes a `[u16 opcode]` into the engine abstraction (a serialization-agnosticism leak). This reshape:

1. Makes the engine a **framed opaque-byte transport** — it frames `[u32 len][opaque bytes]` and knows nothing about opcodes, protobuf, or message types. Type identification moves into the game's payload.
2. Removes the avoidable copies: the two send-side prepend copies, the recv-side framer-accumulate copy, the recv-side drain copy, and the per-recv 64KB allocation churn.
3. Reuses the existing lock-free `NetBufferPool` for both directions (a new instance for send), so send buffers come from a pool instead of per-send `malloc`.

## Current model (as-built, for reference)

### Threads
- **GameThread** — sole caller of the `NetServices` API; single consumer of inbound events (`while (PollEvent(&ev))`).
- **netlib IOCP worker pool** (N per `IocpCore`) — run recv/send completions; inbound *producers*.
- Cross-thread handoffs: inbound via the POD-only `MpscRing` (`MpscRing<T>` requires trivially-copyable `T`, so a `std::vector` cannot travel through it — payload bytes ride in `NetBufferPool`, the ring carries `{poolIndex,len}`); outbound via per-`Conn` `sendQ` (mutex).

### Copy audit
**Send** (GameThread until WSASend):
1. game serialize → bytes — *mandatory*.
2. `NetSubsystem::Send` alloc + memcpy to prepend `[u16 opcode]` — **waste** (full re-copy for 2 bytes).
3. `IocpCore::Send` alloc + memcpy to prepend `[u32 len]` — **waste** (full re-copy for 4 bytes).
4. WSASend → kernel — *mandatory*.

**Recv** (workers A–C, GameThread D–E):
- A. WSARecv → fresh 64KB `op->buffer` — *mandatory* kernel copy, **plus a 64KB heap alloc per recv** (churn).
- B. `Framer::Push` `m_Buf.insert` — **waste** for whole-arrival frames.
- C. `OnAdapterEvent` pool memcpy — *needed* (cross-thread handoff over the POD ring; the worker reuses its recv buffer immediately).
- D. `PollEvent` pool → `m_DrainBuf` memcpy — **waste** (redundant double-buffer for borrow stability).
- E. game parse — *mandatory*.

## Design

### A. New engine contract (serialization-agnostic)

The engine frames opaque bytes only. The `[u16 opcode]` is **removed** from `NetServices`/`NetSubsystem`. Whatever type tag a game needs lives *inside* its opaque payload (this project will use a game-owned `[u16 opcode]` inside the frame; the engine never sees it). The engine wire is exactly:

```
[u32 len][ ...len bytes of opaque payload... ]
 \_____ netlib framing _____/\___ game's bytes ___/
```

**Outbound API (move-based, serialize-in-place):**
```cpp
// Engine hands the caller the FINAL destination buffer (pool block or heap),
// with a reserved 4-byte length head. The caller serializes directly into it.
SendBuffer sb = net->AcquireSend(size_t payloadBytes);
std::byte*  p  = sb.payload();          // writable region AFTER the 4-byte head
// ... caller writes exactly payloadBytes into p ...
net->Send(NetHandle, NetConnId, SendBuffer&& sb);   // netlib writes len into the head, moves to WSASend
```
- `SendBuffer` is a **move-only owned buffer carrying its own release logic** — conceptually `{ std::byte* base; uint32_t cap; Deleter release; }` where the head 4 bytes are reserved and `payload() == base + 4`. It holds either a `NetBufferPool` block (≤ 16 KB payloads; `release` returns the block to the send pool) or a heap buffer (> 16 KB; `release` frees it). The deleter is what lets netlib stay agnostic to *where* the memory came from (see netlib change in §B).
- `AcquireSend(n)` reserves `4 + n` bytes: block from the **send pool** if `4 + n <= 16 KB`, else a heap allocation. `payload()` points at offset 4.
- On `Send`, the transport writes `len = n` into bytes `[0,4)` **in place** (no re-copy) and moves the buffer down to the WSASend op. When that op completes, the buffer's deleter runs — releasing the pool block or freeing the heap allocation — on the IOCP worker thread (safe: pool `Release` is lock-free; `free` is thread-safe).

**Inbound API (opaque borrowed frame):**
```cpp
struct NetEvent {
    NetEventKind   kind;
    NetHandle      adapter;
    NetConnId      conn;
    const std::byte* frame;   // Message only; opaque payload (no opcode field)
    uint32_t       len;       // borrowed until the NEXT PollEvent
};
bool PollEvent(NetEvent* out);
```
- `frame` points **directly into the inbound `NetBufferPool` block** (no `m_DrainBuf`). Valid until the next `PollEvent`, which releases the previously-borrowed block. Game copies if it needs to retain (unchanged contract, just a different backing pointer).

### B. Send path → one copy

`AcquireSend` returns the final destination, so the caller serializes straight into the pool/heap buffer — the serialization result *is* the send buffer; there is no serialize-then-copy. The buffer then travels by **move**: `NetSubsystem::Send` forwards it → `IIo::Send(OwnedBuffer&&)` → `Conn::sendQ` → `IoOp` → WSASend. netlib writes the `[u32 len]` in place into the reserved head, then issues WSASend over `[0, 4+len)`. On completion the `OwnedBuffer`'s deleter releases the backing memory.

- **Copies 2 and 3 eliminated.** Send cost: serialize-in-place + kernel.
- Cost table:

  | Path | malloc | copies |
  |------|--------|--------|
  | send ≤ 16 KB | none (send-pool reuse) | serialize + kernel |
  | send > 16 KB | 1 (heap) | serialize + kernel |

- **netlib change:** introduce a move-only `OwnedBuffer` (`{ std::byte* base; uint32_t cap; type-erased deleter; }`, reserved 4-byte head) in netlib's public headers. `IIoServer::Send` / `IIoClient::Send` change from `std::span<const std::byte>` to `Send(OwnedBuffer&&)`. The caller (engine) constructs the `OwnedBuffer` with a deleter that returns the block to its send pool (or frees the heap buffer); netlib never learns where it came from — it just writes the length prefix into the reserved head, holds the buffer in `IoOp`, WSASends it, and destroys it on completion (running the deleter). `IocpCore::Send` no longer allocates a framed copy. `Conn::sendQ` becomes `deque<OwnedBuffer>` and `IoOp::buffer` becomes an `OwnedBuffer`. Ripples to `TcpClient`, `TcpServer`, `InMemoryAdapter` (the in-memory adapter's deleter-managed buffer is delivered to the peer sink, then released).

### C. Recv path → one handoff copy

- **WSARecv directly into the `Framer`'s persistent buffer tail** instead of a fresh 64 KB `op->buffer` + `insert`. The `Framer` owns a growable `vector<byte>`; the recv op points `wsabuf` at its write cursor; on completion the size advances and frames are scanned in place, consumed prefix compacted (as today). One recv outstanding per `Conn` keeps the address pinned for the in-flight WSARecv.
  - **Eliminates copy B and the per-recv 64 KB allocation.**
- **Copy C stays** — the genuine cross-thread handoff into the inbound `NetBufferPool` block (POD ring; the framer buffer is reused for the next recv on the worker).
- **`PollEvent` exposes the pool block directly** (drop `m_DrainBuf`); release the prior block at the start of the next `PollEvent`. **Eliminates copy D.**
- Recv cost: kernel + pool-handoff + parse.

### D. Allocator reuse

- **Reuse `NetBufferPool`** (`src/common/include/NetBufferPool.h`) — its `Acquire`/`Release` are lock-free (MPMC Vyukov free-list) and already safe across the adapter-worker ↔ GameThread boundary.
  - Inbound: the existing instance (now block-exposed, not drained).
  - Outbound: a **new `NetBufferPool` instance for send buffers**, 16 KB blocks. `AcquireSend` on GameThread; `Release` on the IOCP worker when WSASend completes (lock-free, cross-thread safe).
- The engine `PoolAllocator` / `ArenaAllocator` / `FrameAllocator` are **single-threaded** ("Not thread-safe") and are **not** used here — the net path allocates and frees on different threads. `NetBufferPool` is the only fitting primitive.
- **Optional observability:** register both net pools (or surface their usage via `AllocatorStats`) so send/recv buffer usage appears in the editor Memory panel. `NetBufferPool` is currently off-panel. Nice-to-have, cheap; include if it does not complicate the core reshape.

### E. Oversize handling

- **Send > 16 KB:** `AcquireSend` returns a heap buffer (one `malloc`); serialize-in-place still applies; freed when the WSASend op completes. No copy penalty, only the allocation.
- **Recv > pool block (64 KB inbound, unchanged):** as today — log + drop the frame (oversize guard in `Framer`/`OnAdapterEvent`). The agnostic reshape does not change the inbound block size or the oversize policy.

## Out of scope (explicit)

- Any game-side code: the `[u16 opcode]` codec, `WireCodec` extensions, protobuf, and wiring `NetClientSystem` / `NetServerSystem` to the new API. That is the **follow-up** "before any wiring" boundary the user set. This spec only documents the *contract* the game will use (opaque frame; game owns the opcode inside it).
- Authoritative networking / snapshot replication.
- Scatter-gather WSASend (rejected: needs two pinned buffers per op for equal gain; the single move-down buffer is simpler).
- Killing copy C (rejected: would mean per-message alloc + raw-pointer ownership through the POD ring; the bounded pool is the better trade).

## Result

- **Send:** 3 user copies → **1** (serialize-in-place) + kernel; per-send `malloc` removed for ≤16 KB.
- **Recv:** 3 user copies → **1** (pool handoff) + kernel + parse; per-recv 64 KB alloc removed.
- **Engine fully serialization-agnostic:** wire is `[u32 len][opaque]`; opcode/type lives in the game's payload.

## Decisions locked

- Engine = framed opaque-byte transport; opcode removed from the engine, lives in the game payload.
- Send: `AcquireSend` returns the final destination (pool block ≤16 KB, else heap); serialize-in-place; move down; netlib writes `[u32 len]` in the reserved head.
- Recv: WSARecv into the framer buffer; keep the pool handoff (copy C); expose the pool block in `PollEvent` (drop the drain buffer).
- Reuse `NetBufferPool` for both directions (new 16 KB send-pool instance); engine single-threaded allocators not used.
- Send oversize threshold: **16 KB → heap fallback**.
- Game codec + system wiring are a separate follow-up.
