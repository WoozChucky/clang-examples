# Protobuf wire-format plumbing — design

**Date:** 2026-05-30
**Status:** Approved (brainstorming), pending implementation plan
**Scope:** Plumbing + schema + encode/decode helpers, with representative messages. Not wired into live `Send`/`PollEvent` yet.

## Goal

Introduce Google protobuf (libprotobuf-**lite**) as the wire-format serialization layer for the networking demo in `Game.cpp`. Deliver:

- A vendored protobuf dependency, built from source as a git submodule (matching every other `third_party/` dep).
- A `.proto` schema with a representative set of messages.
- Build-time codegen (`protoc`) producing C++ message classes linked into `Game.dll`.
- Thin encode/decode helpers bridging protobuf to the existing netlib byte API.
- A compiled-but-inert usage example in `Game.cpp` proving the path links and round-trips, **without** changing `NetClientSystem` / `NetServerSystem` runtime behavior.

## Context: the existing wire model

The transport is layered:

- **netlib** frames a byte stream as `[uint32 LE length][payload]` (`src/netlib/src/Framer.h`).
- The **engine NetServices** bridge (`src/common/include/NetServices.h`) defines the payload as `[uint16 opcode][data]`. `NetServices::Send(handle, conn, opcode, data, len)` sends; `PollEvent` yields a `NetEvent{ opcode, payload, len }`.
- Effective wire layout: `[u32 len][u16 opcode][data]`. Today `data` is opaque bytes — the net demo sends a raw `"ping"` and echoes it.

**protobuf changes only the `data` field.** The opcode stays as the message-type discriminator; the length prefix and opcode framing are untouched. No netlib or NetServices change.

## Design

### 1. Dependency / build

- **Pin: latest stable, protobuf `v35.0`** (released 2026-05-19). Confirmed latest via the GitHub releases page before pinning.
- **Direct-dependency submodules (both added by us):**
  - `third_party/protobuf` → `https://github.com/protocolbuffers/protobuf`, tag **`v35.0`**.
  - `third_party/abseil-cpp` → `https://github.com/abseil/abseil-cpp`, tag **`20250512.1`** (the exact abseil version protobuf v35.0 pins in `cmake/dependencies.cmake`).
  - Rationale: protobuf **≥ 22.0 hard-depends on abseil**, and **libprotobuf-lite does not exempt this** — abseil is a base dependency even for the lite runtime. v35's CMake otherwise FetchContent-downloads abseil at configure time; we instead vendor abseil as our own submodule to keep the offline / all-submodules idiom. Per project decision: **direct deps (protobuf, abseil) are submodules we add; any deeper transitive deps may self-fetch/self-configure** — acceptable.
- Wired in `third_party/CMakeLists.txt`:
  - `add_subdirectory(abseil-cpp)` **before** protobuf so abseil's targets already exist and protobuf consumes the local copy (no fetch).
  - `add_subdirectory` of the protobuf tree (exact entry point — repo root vs `protobuf/cmake` — confirmed against the pinned tag during implementation), with options set before the call:
    - `protobuf_BUILD_TESTS = OFF` (avoids the googletest/jsoncpp transitive pulls)
    - `protobuf_BUILD_PROTOC_BINARIES = ON` (we need the `protoc` target for codegen)
    - `protobuf_BUILD_LIBPROTOC = OFF`; build the lite runtime; link target `libprotobuf-lite`.
    - Point protobuf at the local abseil (e.g. via `add_subdirectory` ordering + `CMAKE_PREFIX_PATH`, or `protobuf_ABSL_PROVIDER`/target reuse — exact knob confirmed against v35's CMake during implementation). Optionally `protobuf_LOCAL_DEPENDENCIES_ONLY=ON` to turn any accidental fetch into a hard error.
    - Build shared/static consistent with the rest of the tree (static into `Game.dll` is fine; see hot-reload notes).
- Artifacts consumed: `libprotobuf-lite` (link), `protoc` (codegen tool target), and abseil's targets (transitively, via protobuf-lite).
- **Build cost is notably higher than the original v3.21 plan** — abseil is a large tree. One-off configure/build; flagged so it isn't a surprise.

### 2. Schema

- New file: **`src/game/proto/wire.proto`**.
- `syntax = "proto3";`, `option optimize_for = LITE_RUNTIME;`, `package wire;`.
- Messages (chosen to exercise scalars, nested message, repeated, enum):

```proto
syntax = "proto3";
package wire;
option optimize_for = LITE_RUNTIME;

enum Opcode {
  OPCODE_UNSPECIFIED = 0;
  OPCODE_PING        = 1;
  OPCODE_PONG        = 2;
  OPCODE_SNAPSHOT    = 3;
}

message Ping  { uint32 seq = 1; uint64 client_time_ms = 2; }
message Pong  { uint32 seq = 1; uint64 server_time_ms = 2; }
message Vec3  { float x = 1; float y = 2; float z = 3; }
message PlayerState { uint64 entity_id = 1; Vec3 position = 2; float yaw = 3; }
message Snapshot    { uint32 tick = 1; repeated PlayerState players = 2; }
```

- The `Opcode` enum values match the existing `u16 opcode` (Ping echo demo already uses 1/2).

### 3. Codegen + codec helpers

- **CMake custom command** in `src/game/CMakeLists.txt`:
  - Runs `$<TARGET_FILE:protoc> --cpp_out=<gen-dir> -I src/game/proto src/game/proto/wire.proto`.
  - Output: `wire.pb.h` / `wire.pb.cc` into a generated dir under the build tree (e.g. `${CMAKE_CURRENT_BINARY_DIR}/gen`).
  - `wire.pb.cc` added to the `game` target sources; gen-dir added to the target include dirs; `target_link_libraries(game PRIVATE libprotobuf-lite)`; depends on the `protoc` target so codegen waits for the compiler build.
- **New header `src/game/src/WireCodec.h`** (header-only, game-local) — thin wrappers over the protobuf-lite serialize/parse API:
  - `template<class M> std::vector<uint8_t> Encode(const M& msg);` → `msg.SerializeToArray(...)` into a sized buffer.
  - `template<class M> bool Decode(const uint8_t* data, uint32_t len, M& out);` → `out.ParseFromArray(data, len)`.
  - `template<class M> wire::Opcode OpcodeOf();` trait specialized per message type so a send-site picks the right opcode (e.g. `OpcodeOf<wire::Ping>() == OPCODE_PING`).

### 4. Game.cpp usage (demo, not live-wired)

- Add a small, self-contained example in `Game.cpp` that compiles and links against the generated code and `WireCodec.h`: build a `wire::Ping`, `Encode` it, `Decode` it back, assert/log round-trip equality. Kept inert with respect to the live net systems (a standalone helper invoked once, or guarded so it does not alter `NetClientSystem`/`NetServerSystem` send/recv behavior).
- This satisfies the "just the plumbing" scope: proves protobuf encode/decode works over the project's byte API without committing the demo to a new on-wire format yet.

### 5. Hot-reload / risks

- `libprotobuf-lite` + abseil + generated code link **into `Game.dll`**. Message objects are **per-tick scratch** — never stored in editor-owned `GameState` — so a hot-reload that destroys/recreates systems is reload-safe.
- **Risk to watch:** protobuf and abseil run static initialization (one-time registration, abseil flags/once-init) on DLL load. This state is fully contained in the DLL (unloaded with it), so repeated hot-reloads should be clean — but the abseil surface is larger than the original lite-only plan, so explicitly verify on the first reload after integration.
- **Build cost:** first configure/build compiles abseil + protobuf + `protoc` once. Heavier than the dropped v3.21 plan (abseil); one-off, acceptable.
- **`.proto` changes** are codegen, not the `Game.h` ABI — editing `wire.proto` rebuilds `wire.pb.*` into `Game.dll`; a `.cpp`-only Game change still hot-reloads. Changing the schema does **not** require a `GAME_API_VERSION` bump (no `Game.h`/`GameState` layout change).

## Out of scope

- Wiring protobuf into the live `NetClientSystem` / `NetServerSystem` send/recv path (future work — would replace the raw `"ping"` payload with `wire::Ping`).
- Sharing the schema with `server.exe` via `src/common/` (proto stays game-owned per decision; `Game.dll` is already the shared net code both `editor` and `server` load).
- Full reflection / JSON / descriptor features (lite runtime only). Note: abseil is **not** out of scope — it is a mandatory base dependency of modern protobuf, vendored as a submodule per section 1.

## Decisions locked

- Runtime: **libprotobuf-lite**, protobuf **v35.0** (latest stable, confirmed online).
- Dependencies as **git submodules** for direct deps: `third_party/protobuf` (`v35.0`) + `third_party/abseil-cpp` (`20250512.1`). Deeper transitive deps may self-fetch.
- Proto location: **`src/game/proto/wire.proto`** (game-owned).
