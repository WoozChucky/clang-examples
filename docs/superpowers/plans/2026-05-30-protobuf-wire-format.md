# Protobuf Wire-Format Plumbing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Vendor Google protobuf (libprotobuf-lite, v35.0) as a build dependency, add a game-owned `wire.proto` schema with build-time `protoc` codegen, thin encode/decode helpers, and an inert round-trip smoke test in `Game.cpp` — without changing the live net systems.

**Architecture:** protobuf is added as a single git submodule under `third_party/`; its CMake fetches abseil (and other transitive deps) itself at configure time. `protoc` (built from that submodule) generates `wire.pb.{h,cc}` into the build tree; those plus `libprotobuf-lite` link into `Game.dll`. A header-only `WireCodec.h` wraps protobuf's serialize/parse API over the netlib `(data,len)` byte convention. The wire framing (`[u32 len][u16 opcode][data]`) is unchanged — protobuf only fills `data`.

**Tech Stack:** C++23, CMake, Google protobuf v35.0 (libprotobuf-lite), `protoc`, MSVC (Enterprise preset).

**Build/verify commands (this repo):**
- Configure: `cmake --preset msvc-win64-vs2026-enterprise`
- Build game: `cmake --build out/build/msvc-win64-vs2026-enterprise --target game`
- Binaries: `out/build/msvc-win64-vs2026-enterprise/bin/Debug/`

**Reference:** spec at `docs/superpowers/specs/2026-05-30-protobuf-wire-format-design.md`.

> **Note on TDD:** the `game` target has no unit-test harness (tests exist only for `ecs`/`alloc`). The executable test for this feature is an `SM_ASSERT` round-trip check that runs once at game boot (Task 6) — it fails loudly (MessageBox + debug break) if encode/decode is wrong. Build success is the test for the codegen/link tasks.

---

### Task 1: Add the protobuf submodule

**Files:**
- Modify: `.gitmodules` (via `git submodule add`)
- Create: `third_party/protobuf/` (submodule checkout)

- [ ] **Step 1: Add the submodule pinned to v35.0**

Run from repo root:
```bash
git submodule add https://github.com/protocolbuffers/protobuf third_party/protobuf
git -C third_party/protobuf fetch --tags --depth 1 origin v35.0
git -C third_party/protobuf checkout v35.0
```
(abseil is NOT a submodule — protobuf's CMake fetches it at configure time. No `--recursive` needed; protobuf v35's `.gitmodules` is empty.)

- [ ] **Step 2: Verify the checkout**

Run:
```bash
git -C third_party/protobuf describe --tags
```
Expected: `v35.0` (or `v35.0` with a trailing commit suffix if not exact-match; the `git checkout v35.0` above guarantees the tag).

Confirm the CMake entry point exists:
```bash
test -f third_party/protobuf/CMakeLists.txt && echo "ROOT CMake present"
```
Expected: `ROOT CMake present`. (If absent, the entry point is `third_party/protobuf/cmake` — note it for Task 2.)

- [ ] **Step 3: Commit**

```bash
git add .gitmodules third_party/protobuf
git commit -m "build(net): add protobuf v35.0 submodule"
```

---

### Task 2: Wire protobuf into the third_party build

**Files:**
- Modify: `third_party/CMakeLists.txt` (append a protobuf block at the end)

- [ ] **Step 1: Append the protobuf block**

Add to the END of `third_party/CMakeLists.txt`:
```cmake
# Protocol Buffers (lite runtime). abseil and other transitive deps are fetched
# by protobuf's own CMake at configure time (network required on first configure).
# Static libs (no BUILD_SHARED_LIBS) link into Game.dll; force /MD to match the
# globally-forced MultiThreadedDLL runtime (protobuf defaults to /MT otherwise).
set(protobuf_BUILD_TESTS           OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_PROTOC_BINARIES ON  CACHE BOOL "" FORCE)
set(protobuf_BUILD_LIBPROTOC       OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL               OFF CACHE BOOL "" FORCE)
set(protobuf_MSVC_STATIC_RUNTIME   OFF CACHE BOOL "" FORCE)
add_subdirectory(protobuf)
# (If third_party/protobuf/CMakeLists.txt was absent in Task 1, use:
#  add_subdirectory(protobuf/cmake) instead of the line above.)

# Folder org for the protobuf targets defined directly in that dir.
get_directory_property(protobuf_targets DIRECTORY protobuf BUILDSYSTEM_TARGETS)
foreach(target ${protobuf_targets})
    set_target_properties(${target} PROPERTIES FOLDER ThirdParty/protobuf)
endforeach()
```

- [ ] **Step 2: Configure (fetches abseil — needs network)**

Run:
```bash
cmake --preset msvc-win64-vs2026-enterprise
```
Expected: configure completes without error; log shows abseil being fetched/populated by protobuf. First run is slow.

- [ ] **Step 3: Verify the protobuf targets exist**

Run:
```bash
cmake --build out/build/msvc-win64-vs2026-enterprise --target libprotobuf-lite
cmake --build out/build/msvc-win64-vs2026-enterprise --target protoc
```
Expected: both build successfully (this also builds abseil the first time).

- [ ] **Step 4: Commit**

```bash
git add third_party/CMakeLists.txt
git commit -m "build(net): build protobuf-lite + protoc from the submodule"
```

---

### Task 3: Author the wire schema

**Files:**
- Create: `src/game/proto/wire.proto`

- [ ] **Step 1: Write the schema**

Create `src/game/proto/wire.proto`:
```proto
syntax = "proto3";

package wire;

option optimize_for = LITE_RUNTIME;

// Message-type discriminator carried in the existing [u16 opcode] frame field.
// Values match the net demo's current opcodes (ping=1 / echo=2).
enum Opcode {
  OPCODE_UNSPECIFIED = 0;
  OPCODE_PING        = 1;
  OPCODE_PONG        = 2;
  OPCODE_SNAPSHOT    = 3;
}

message Ping {
  uint32 seq            = 1;
  uint64 client_time_ms = 2;
}

message Pong {
  uint32 seq            = 1;
  uint64 server_time_ms = 2;
}

message Vec3 {
  float x = 1;
  float y = 2;
  float z = 3;
}

message PlayerState {
  uint64 entity_id = 1;
  Vec3   position  = 2;
  float  yaw       = 3;
}

message Snapshot {
  uint32              tick    = 1;
  repeated PlayerState players = 2;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/game/proto/wire.proto
git commit -m "feat(net): add wire.proto schema (ping/pong/snapshot)"
```

---

### Task 4: Codegen + link protobuf into the game target

**Files:**
- Modify: `src/game/CMakeLists.txt`

- [ ] **Step 1: Add the codegen + link block**

In `src/game/CMakeLists.txt`, AFTER the `add_library(game SHARED src/Game.cpp)` block (around line 44) and BEFORE the `set_target_properties(game ...)` block, insert:
```cmake
# --- protobuf wire-format codegen ---
# protoc (built from the third_party/protobuf submodule) generates wire.pb.{h,cc}
# into the build tree; the .cc compiles into Game.dll and links libprotobuf-lite.
set(GAME_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(GAME_PROTO_GEN "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${GAME_PROTO_GEN}")
set(GAME_PROTO_CC  "${GAME_PROTO_GEN}/wire.pb.cc")
set(GAME_PROTO_H   "${GAME_PROTO_GEN}/wire.pb.h")

add_custom_command(
    OUTPUT  "${GAME_PROTO_CC}" "${GAME_PROTO_H}"
    COMMAND protobuf::protoc
            --cpp_out "${GAME_PROTO_GEN}"
            -I "${GAME_PROTO_DIR}"
            "${GAME_PROTO_DIR}/wire.proto"
    DEPENDS "${GAME_PROTO_DIR}/wire.proto" protobuf::protoc
    COMMENT "protoc: generating wire.pb.{h,cc}"
    VERBATIM
)

target_sources(game PRIVATE "${GAME_PROTO_CC}")
target_include_directories(game PRIVATE "${GAME_PROTO_GEN}")
target_link_libraries(game PRIVATE protobuf::libprotobuf-lite)
```

- [ ] **Step 2: Reconfigure**

Run:
```bash
cmake --preset msvc-win64-vs2026-enterprise
```
Expected: configure succeeds; the `game` target now depends on the codegen command.

- [ ] **Step 3: Build game — verify codegen runs and links**

Run:
```bash
cmake --build out/build/msvc-win64-vs2026-enterprise --target game
```
Expected: log shows `protoc: generating wire.pb.{h,cc}`, then `Game.cpp` / `wire.pb.cc` compile, then `Game.dll` links with no unresolved-symbol errors.

- [ ] **Step 4: Verify generated files exist**

Run:
```bash
test -f out/build/msvc-win64-vs2026-enterprise/src/game/gen/wire.pb.h && echo "header generated"
```
Expected: `header generated`.

- [ ] **Step 5: Commit**

```bash
git add src/game/CMakeLists.txt
git commit -m "build(net): protoc codegen for wire.proto, link protobuf-lite into Game.dll"
```

---

### Task 5: Encode/decode helpers

**Files:**
- Create: `src/game/src/WireCodec.h`

- [ ] **Step 1: Write the codec header**

Create `src/game/src/WireCodec.h`:
```cpp
#pragma once

// Thin helpers bridging protobuf-lite messages to the netlib byte API
// (NetServices::Send(handle, conn, opcode, data, len) / NetEvent{payload,len}).
// Header-only, game-local. Wire framing is unchanged — these only (de)serialize
// the `data` field of an existing [u16 opcode][data] frame.

#include <cstdint>
#include <vector>

#include "wire.pb.h"   // generated into the build tree (gen/), on the game include path

namespace wirecodec {

// Serialize a protobuf-lite message to a byte buffer suitable for Send(data, len).
template <class M>
std::vector<uint8_t> Encode(const M& msg) {
    std::vector<uint8_t> out(msg.ByteSizeLong());
    if (!out.empty())
        msg.SerializeToArray(out.data(), static_cast<int>(out.size()));
    return out;
}

// Parse a NetEvent payload into a message. Returns false on malformed/truncated input.
template <class M>
bool Decode(const uint8_t* data, uint32_t len, M& out) {
    return out.ParseFromArray(data, static_cast<int>(len));
}

// Opcode trait: maps a message type to the wire::Opcode tag a send-site passes as
// the frame opcode. Specialize one line per message type.
template <class M> wire::Opcode OpcodeOf();
template <> inline wire::Opcode OpcodeOf<wire::Ping>()     { return wire::OPCODE_PING; }
template <> inline wire::Opcode OpcodeOf<wire::Pong>()     { return wire::OPCODE_PONG; }
template <> inline wire::Opcode OpcodeOf<wire::Snapshot>() { return wire::OPCODE_SNAPSHOT; }

} // namespace wirecodec
```

- [ ] **Step 2: Commit**

```bash
git add src/game/src/WireCodec.h
git commit -m "feat(net): WireCodec encode/decode + opcode trait helpers"
```

---

### Task 6: Inert round-trip smoke test in Game.cpp

**Files:**
- Modify: `src/game/src/Game.cpp` (add include near the top; add a smoke-test block in the one-time boot)

- [ ] **Step 1: Add the include**

In `src/game/src/Game.cpp`, add after the existing `#include "Atmosphere.h"` line (line 12):
```cpp
#include "WireCodec.h"  // protobuf wire-format encode/decode (plumbing demo)
```

- [ ] **Step 2: Add the smoke-test block**

In `GameUpdate`, inside the one-time boot `if (g_GameState->StateId == GameStateId::Uninitialized) {` block, immediately after `SM_TRACE("[GAMEDLL] Initializing game...");` (line 635), insert:
```cpp
        // One-time protobuf wire-format smoke test. Proves the protoc-generated
        // code + libprotobuf-lite link and round-trip over the byte API. NOT wired
        // into the live net systems — this is plumbing validation only.
        {
            wire::Ping ping;
            ping.set_seq(42);
            ping.set_client_time_ms(123456);

            const std::vector<uint8_t> bytes = wirecodec::Encode(ping);

            wire::Ping back;
            const bool ok = wirecodec::Decode(bytes.data(),
                                              static_cast<uint32_t>(bytes.size()), back);
            SM_ASSERT(ok && back.seq() == 42u && back.client_time_ms() == 123456ull,
                      "protobuf wire round-trip failed");
            SM_TRACE("[GAMEDLL] protobuf smoke test OK: Ping seq=%u, %zu bytes, opcode=%d",
                     back.seq(), bytes.size(),
                     static_cast<int>(wirecodec::OpcodeOf<wire::Ping>()));
        }
```

- [ ] **Step 3: Build game**

Run:
```bash
cmake --build out/build/msvc-win64-vs2026-enterprise --target game
```
Expected: `Game.dll` builds clean (compiles `WireCodec.h` + the smoke block against `wire.pb.h`).

- [ ] **Step 4: Run and observe the round-trip log**

Run the editor:
```bash
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/editor.exe
```
Expected: on startup, the console logs:
`[GAMEDLL] protobuf smoke test OK: Ping seq=42, <N> bytes, opcode=1`
and NO assertion MessageBox. (Close the editor after confirming.)

- [ ] **Step 5: Verify hot-reload is clean (spec risk check)**

With the editor still running from Step 4, re-run the game build to trigger a hot-reload:
```bash
cmake --build out/build/msvc-win64-vs2026-enterprise --target game
```
Expected: editor logs the DLL reload (new timestamped `Game_load_*.dll`) within ~1s, NO crash, NO assert. The smoke test does not re-run on reload (boot-only), but the reload exercising the protobuf/abseil static-init surface confirms the hot-reload risk flagged in the spec is benign.

- [ ] **Step 6: Commit**

```bash
git add src/game/src/Game.cpp
git commit -m "feat(net): inert protobuf wire round-trip smoke test at game boot"
```

---

## Self-Review

**Spec coverage:**
- Submodule (single, protobuf v35.0; abseil self-fetched) → Task 1, Task 2.
- CMake options (`BUILD_TESTS=OFF`, `PROTOC_BINARIES=ON`, `LIBPROTOC=OFF`, lite, `MSVC_STATIC_RUNTIME=OFF`) → Task 2 Step 1.
- `src/game/proto/wire.proto` with Opcode/Ping/Pong/Vec3/PlayerState/Snapshot → Task 3.
- protoc codegen custom command + gen-dir include + `libprotobuf-lite` link → Task 4.
- `WireCodec.h` Encode/Decode/OpcodeOf → Task 5.
- Inert Game.cpp demo, round-trip, not live-wired → Task 6.
- Hot-reload risk verification → Task 6 Step 5.
- No netlib/NetServices change → respected (nothing in the plan touches them).

**Type consistency:** `wirecodec::Encode`/`Decode`/`OpcodeOf`, `wire::Ping`/`Pong`/`Snapshot`/`OPCODE_*` names match across Tasks 3, 5, 6. Targets `protobuf::protoc` / `protobuf::libprotobuf-lite` consistent across Tasks 2 and 4.

**Placeholder scan:** none — all steps carry concrete code/commands.

**Open implementation-time confirmations (flagged inline, not placeholders):**
- protobuf v35 CMake entry point (`protobuf` vs `protobuf/cmake`) — Task 1 Step 2 / Task 2 Step 1 note.
- Exported target names `protobuf::protoc` / `protobuf::libprotobuf-lite` are protobuf's standard CMake exports; if a version-specific alias differs, the configure/build in Task 2 Step 3 surfaces it before any game wiring.
