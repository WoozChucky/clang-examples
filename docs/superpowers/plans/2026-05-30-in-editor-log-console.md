# In-Editor Log Console Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A dockable editor **Console** panel that shows the running application's `SM_TRACE/WARN/ERROR` logs live — from every in-process module (editor.exe, Engine.dll, ecs.dll, Game.dll, netlib.dll) and every thread — filterable by level and text, color-coded, without losing stdout output.

**Architecture:** Add a per-module log-sink hook to the header-inlined `_log` (in `lib.h`); each capturable module exports a one-line installer that sets *its own* sink pointer; the editor installs the same sink (a free function pushing into a process-global lock-free `LogBus` ring) into all modules at startup, and `GameLibrary` re-installs into `Game.dll` after every hot-reload. A `ConsolePanel` (RenderThread) drains the ring each frame into a bounded scrollback and renders it with ImGui.

**Tech Stack:** C++23, existing `MpscRing` (Vyukov, `src/common/include/MpscRing.h`), the per-module `ENGINE_API`/`ECS_API`/`NETLIB_API`/`EXPORT_FN` export macros, ImGui (editor panels), CMake preset `msvc-win64-vs2026-community`.

**Spec:** `docs/superpowers/specs/2026-05-30-in-editor-log-console-design.md`

**Conventions (project memory):**
- Build/test ONLY with `msvc-win64-vs2026-community`. Configure: `cmake --preset msvc-win64-vs2026-community`. Build a target: `cmake --build --preset msvc-win64-vs2026-community --target <t>`. Test exes run from `out/build/msvc-win64-vs2026-community/bin/Debug/`.
- Commit as `git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "..."`. NEVER `--no-verify`. Do NOT push. Work stays on branch `feat/log-console` (already checked out) — do not branch or merge.
- **Git path-case gotcha:** stage with the exact case `git status` shows (e.g. the repo tracks `src/game/include/game.h` lowercase, `src/game/src/game.cpp`); after committing run `git status --porcelain` (clean) + `git show HEAD --stat` (all intended files present).
- `lib.h` is included by every module → a change to it forces a **rebuild of all targets and an editor restart**. Expected, not a regression. No `GAME_API_VERSION` bump (this is not a `GameState`/export-signature change).

---

## File Structure

**New files:**
- `src/editor/src/app/LogBus.h` / `LogBus.cpp` — `LogRecord`, the `LogBus` singleton (owns `MpscRing<LogRecord>`, drop counter, seq counter), and the free `ConsoleLogSink` function installed into every module. No ImGui.
- `src/editor/src/panels/ConsolePanel.h` / `ConsolePanel.cpp` — `ConsolePanel` class: per-frame drain of `LogBus` into a bounded scrollback deque + filter/search/autoscroll UI state + ImGui render.
- `src/engine/include/EngineLogSink.h` + `src/engine/src/core/EngineLogSink.cpp` — `EngineInstallLogSink` (ENGINE_API).
- `src/common/include/EcsLogSink.h` + `src/ecs/src/EcsLogSink.cpp` — `EcsInstallLogSink` (ECS_API).
- `src/netlib/include/netlib/log_sink.h` + `src/netlib/src/LogSink.cpp` — `netlib::SetLogSink` (NETLIB_API).
- `tests/test_logbus.cpp` — unit tests for `LogBus` push/drain/truncate/overflow.

**Modified files:**
- `src/common/include/LogFormat.h` — add `format_log_body` (plain, no ANSI).
- `src/common/include/lib.h` — `LogLevel` enum, `LogSinkFn`, `g_SmLogSink`, `sm_set_log_sink`, `_log` gains a `LogLevel` param + sink call, macros pass level, `sm_assert_fail` passes `LogLevel::Error`.
- `tests/test_logformat.cpp` — assert `format_log_body` has no ANSI; assert the sink hook delivers level+plain-text.
- `src/engine/src/threading/GameLibrary.cpp` — after a successful (re)load, resolve + call `GameInstallLogSink(g_SmLogSink)`.
- `src/game/src/game.cpp` — export `GameInstallLogSink`.
- `src/editor/src/main.cpp` — install the sink into editor.exe + Engine + ecs + netlib at startup.
- `src/editor/src/app/ImGuiRenderer.h` / `ImGuiRenderer.cpp` — own a `ConsolePanel`, draw it.
- `src/editor/src/panels/MainMenuBar.cpp` — Console window toggle (if the menu pattern is simple; else default-visible).
- `src/engine/CMakeLists.txt`, `src/ecs/CMakeLists.txt`, `src/netlib/CMakeLists.txt`, `src/editor/CMakeLists.txt`, `tests/CMakeLists.txt` — add the new sources/targets.

---

## Task 1: Logging core — `LogLevel`, plain-body formatter, sink hook

**Files:**
- Modify: `src/common/include/LogFormat.h`
- Modify: `src/common/include/lib.h`
- Modify: `tests/test_logformat.cpp`

This is the foundation: a sink hook in the header-inlined `_log`, with **no behavior change when no sink is installed** (runtime/server stay byte-identical).

- [ ] **Step 1: Write the failing tests** — append to `tests/test_logformat.cpp` (before `main`), and call them from `main`.

```cpp
// format_log_body produces the PLAIN "<prefix> <body>" with NO ANSI escape codes.
static void T04_body_no_ansi()
{
    const std::string b = format_log_body("WARN:", "n=%d", 7);
    EXPECT(b == std::string("WARN: n=7"));
    EXPECT(b.find('\x1b') == std::string::npos);   // no color escape
    EXPECT(b.find("\033[0m") == std::string::npos); // no reset
}

// The sink hook: when a sink is installed, _log delivers (level, plain text); when
// cleared, it is not called. (Exercised through the SM_ macros.)
static int   g_SinkCalls = 0;
static LogLevel g_SinkLevel = LogLevel::Trace;
static std::string g_SinkText;
static void TestSink(LogLevel lvl, TextColor /*c*/, const char* text) {
    ++g_SinkCalls; g_SinkLevel = lvl; g_SinkText = text;
}
static void T05_sink_hook()
{
    g_SinkCalls = 0;
    sm_set_log_sink(&TestSink);
    SM_WARN("hello %d", 42);
    EXPECT(g_SinkCalls == 1);
    EXPECT(g_SinkLevel == LogLevel::Warn);
    EXPECT(g_SinkText == std::string("WARN: hello 42"));   // plain, no ANSI

    sm_set_log_sink(nullptr);
    SM_ERROR("ignored");
    EXPECT(g_SinkCalls == 1);   // not called after clear
}
```
Add `#include "lib.h"` to the test's includes (it currently includes only `LogFormat.h`), and add `T04_body_no_ansi(); T05_sink_hook();` to `main()`.

- [ ] **Step 2: Run to verify it fails**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_logformat
```
Expected: COMPILE error (`format_log_body`, `LogLevel`, `sm_set_log_sink` undefined).

- [ ] **Step 3: Add `format_log_body` to `LogFormat.h`** (next to `format_log_line`)

```cpp
// Plain "<prefix> <body>" — no color codes. For sinks (editor console) that colorize
// themselves. format_log_line stays the ANSI-wrapped variant for stdout.
template <typename... Args>
std::string format_log_body(const char* prefix, const char* msg, Args... args) {
    char body[8192];
    std::snprintf(body, sizeof(body), msg, args...);
    std::string out;
    out += prefix; out += ' '; out += body;
    return out;
}
```

- [ ] **Step 4: Add the level enum + sink hook to `lib.h`** (replace the logging block at lines ~88–96)

```cpp
enum class LogLevel : uint8_t { Trace, Warn, Error };

// Per-module sink (lib.h is header-inlined, so each DLL/exe gets its own copy — intended;
// the editor installs into each module via that module's exported installer). Set null in
// runtime/server → stdout-only, unchanged.
using LogSinkFn = void(*)(LogLevel level, TextColor color, const char* text);
inline LogSinkFn g_SmLogSink = nullptr;
inline void sm_set_log_sink(LogSinkFn fn) { g_SmLogSink = fn; }

template <typename... Args>
void _log(LogLevel level, const char* prefix, const char* msg, TextColor color, Args... args)
{
    std::println("{}", format_log_line(prefix, msg, color, args...));   // stdout (ANSI), unchanged
    if (g_SmLogSink) {
        const std::string body = format_log_body(prefix, msg, args...); // plain, no ANSI
        g_SmLogSink(level, color, body.c_str());
    }
}

#define SM_TRACE(msg, ...) do { _log(LogLevel::Trace, "TRACE:", msg, TEXT_COLOR_GREEN,  ##__VA_ARGS__); } while(0)
#define SM_WARN(msg, ...)  do { _log(LogLevel::Warn,  "WARN:",  msg, TEXT_COLOR_YELLOW, ##__VA_ARGS__); } while(0)
#define SM_ERROR(msg, ...) do { _log(LogLevel::Error, "ERROR:", msg, TEXT_COLOR_RED,    ##__VA_ARGS__); } while(0)
```

- [ ] **Step 5: Update `sm_assert_fail` in `lib.h`** — its internal `_log("ERROR:", "%s", ...)` call now needs the level param. Change:

```cpp
  _log(LogLevel::Error, "ERROR:", "%s", TEXT_COLOR_RED, formatted);
```

- [ ] **Step 6: Run the test, verify it passes**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_logformat
./out/build/msvc-win64-vs2026-community/bin/Debug/test_logformat.exe
```
Expected: `All log format tests passed.`

- [ ] **Step 7: Sanity-build a couple of dependent targets** (confirm the `_log` signature change compiles everywhere it's used)

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target ecs --target Engine
```
Expected: both build clean (no `_log` call-site breakage; the only direct `_log` caller is `sm_assert_fail`, already fixed — everything else goes through the macros).

- [ ] **Step 8: Commit**

```bash
git add src/common/include/LogFormat.h src/common/include/lib.h tests/test_logformat.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(log): LogLevel + plain-body formatter + per-module sink hook in lib.h"
```

---

## Task 2: `LogBus` + `LogRecord` + `ConsoleLogSink` (+ unit test)

**Files:**
- Create: `src/editor/src/app/LogBus.h`, `src/editor/src/app/LogBus.cpp`
- Create: `tests/test_logbus.cpp`
- Modify: `tests/CMakeLists.txt`

The process-global, lock-free bridge between "a log happened (any thread)" and "the panel reads it (RenderThread)".

- [ ] **Step 1: Write the failing test** — `tests/test_logbus.cpp`

```cpp
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "LogBus.h"

static int g_Failures = 0;
#define EXPECT(c) do { if(!(c)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); ++g_Failures; } } while(0)

int main() {
    LogBus& bus = LogBus::Instance();

    // Round-trip: push 3, drain 3, order + level + text preserved, seq increments.
    bus.Push(LogLevel::Trace, TEXT_COLOR_GREEN,  "first");
    bus.Push(LogLevel::Warn,  TEXT_COLOR_YELLOW, "second");
    bus.Push(LogLevel::Error, TEXT_COLOR_RED,    "third");

    LogRecord out[8];
    size_t n = bus.Drain(out, 8);
    EXPECT(n == 3);
    EXPECT(out[0].level == LogLevel::Trace && std::string(out[0].text) == "first");
    EXPECT(out[1].level == LogLevel::Warn  && std::string(out[1].text) == "second");
    EXPECT(out[2].level == LogLevel::Error && std::string(out[2].text) == "third");
    EXPECT(out[0].seq < out[1].seq && out[1].seq < out[2].seq);

    // Drain on empty returns 0.
    EXPECT(bus.Drain(out, 8) == 0);

    // Truncation: an over-long message is cut to kLogTextMax-1 chars + NUL, with a trailing "…".
    std::string big(1000, 'x');
    bus.Push(LogLevel::Trace, TEXT_COLOR_GREEN, big.c_str());
    n = bus.Drain(out, 8);
    EXPECT(n == 1);
    EXPECT(std::strlen(out[0].text) == kLogTextMax - 1);

    // Overflow: push more than ring capacity without draining → DroppedCount climbs, no crash.
    const uint64_t before = bus.DroppedCount();
    for (int i = 0; i < 9000; ++i) bus.Push(LogLevel::Trace, TEXT_COLOR_GREEN, "flood");
    EXPECT(bus.DroppedCount() > before);
    // drain whatever fit (must not crash / hang)
    while (bus.Drain(out, 8) > 0) {}

    if (g_Failures == 0) { std::printf("All logbus tests passed.\n"); return 0; }
    std::printf("%d logbus test(s) FAILED.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 2: Create `LogBus.h`**

```cpp
#pragma once
#include <atomic>
#include <cstdint>
#include "lib.h"        // LogLevel, TextColor (via LogFormat.h)
#include "MpscRing.h"

inline constexpr int kLogTextMax = 240;

// Trivially-copyable so it lives directly in MpscRing (no payload pool).
struct LogRecord {
    LogLevel level;
    uint8_t  color;                 // TextColor, for display
    uint32_t seq;                   // monotonic
    double   t;                     // steady_clock seconds at push
    char     text[kLogTextMax];     // plain body; truncated + "…" if longer
};

// Process-global log ring. Producers: any thread (the installed sink). Consumer: the
// ConsolePanel drain (RenderThread). Lock-free; overflow drops newest + bumps a counter.
class LogBus {
public:
    static LogBus& Instance();

    void     Push(LogLevel level, TextColor color, const char* text);   // any thread
    size_t   Drain(LogRecord* out, size_t maxN);                        // single consumer
    uint64_t DroppedCount() const { return m_Dropped.load(std::memory_order_relaxed); }

private:
    LogBus() = default;
    static constexpr size_t kRingSize = 8192;
    MpscRing<LogRecord, kRingSize> m_Ring;
    std::atomic<uint64_t> m_Dropped{0};
    std::atomic<uint32_t> m_Seq{0};
};

// The free sink function installed into every module (no capture → plain fn-ptr).
void ConsoleLogSink(LogLevel level, TextColor color, const char* text);
```

- [ ] **Step 3: Create `LogBus.cpp`**

```cpp
#include "LogBus.h"
#include <chrono>
#include <cstring>

LogBus& LogBus::Instance() {
    static LogBus s_Instance;
    return s_Instance;
}

void LogBus::Push(LogLevel level, TextColor color, const char* text) {
    LogRecord r{};
    r.level = level;
    r.color = static_cast<uint8_t>(color);
    r.seq   = m_Seq.fetch_add(1, std::memory_order_relaxed);
    r.t     = std::chrono::duration<double>(
                  std::chrono::steady_clock::now().time_since_epoch()).count();

    // Copy + truncate (reserve 1 byte for NUL; mark truncation with a trailing "…").
    const size_t cap = kLogTextMax - 1;
    size_t n = 0;
    if (text) { n = std::strlen(text); if (n > cap) n = cap; std::memcpy(r.text, text, n); }
    if (text && std::strlen(text) > cap && cap >= 3) {
        // overwrite the last 3 bytes with an ellipsis marker "..."
        std::memcpy(r.text + cap - 3, "...", 3);
    }
    r.text[n] = '\0';

    if (!m_Ring.Enqueue(r)) {
        m_Dropped.fetch_add(1, std::memory_order_relaxed);   // ring full → drop newest, surfaced
    }
}

size_t LogBus::Drain(LogRecord* out, size_t maxN) {
    size_t n = 0;
    LogRecord r;
    while (n < maxN && m_Ring.Dequeue(r)) out[n++] = r;
    return n;
}

void ConsoleLogSink(LogLevel level, TextColor color, const char* text) {
    LogBus::Instance().Push(level, color, text);
}
```

> Engineer note: confirm the `MpscRing` method names (`Enqueue`/`Dequeue`) against `src/common/include/MpscRing.h` (NetBufferPool uses `m_Free->Enqueue(i)` / `m_Free->Dequeue(idx)`, so these are correct). `MpscRing` `static_assert`s trivially-copyable — `LogRecord` satisfies it (PODs + `char[]`).

- [ ] **Step 4: Add the test target** — append to `tests/CMakeLists.txt`

```cmake
add_executable(test_logbus
    test_logbus.cpp
    ${CMAKE_SOURCE_DIR}/src/editor/src/app/LogBus.cpp
)

target_include_directories(test_logbus PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/editor/src/app
)

set_target_properties(test_logbus PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 5: Build + run, verify pass**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_logbus
./out/build/msvc-win64-vs2026-community/bin/Debug/test_logbus.exe
```
Expected: `All logbus tests passed.`

- [ ] **Step 6: Commit**

```bash
git add src/editor/src/app/LogBus.h src/editor/src/app/LogBus.cpp tests/test_logbus.cpp tests/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(log): LogBus lock-free ring + LogRecord + ConsoleLogSink"
```

---

## Task 3: Per-module install entry points (Engine / ecs / netlib / Game) + GameLibrary forward

**Files:**
- Create: `src/engine/include/EngineLogSink.h`, `src/engine/src/core/EngineLogSink.cpp`
- Create: `src/common/include/EcsLogSink.h`, `src/ecs/src/EcsLogSink.cpp`
- Create: `src/netlib/include/netlib/log_sink.h`, `src/netlib/src/LogSink.cpp`
- Modify: `src/game/src/game.cpp` (export `GameInstallLogSink`)
- Modify: `src/engine/src/threading/GameLibrary.cpp` (forward sink after load)
- Modify: `src/engine/CMakeLists.txt`, `src/ecs/CMakeLists.txt`, `src/netlib/CMakeLists.txt`

Each capturable module gets ONE exported one-liner that sets *its own* `g_SmLogSink`. (Only code compiled into a module can set that module's header-inlined copy.)

- [ ] **Step 1: Engine installer**

`src/engine/include/EngineLogSink.h`:
```cpp
#pragma once
#include "Engine.h"   // ENGINE_API
#include "lib.h"      // LogSinkFn
ENGINE_API void EngineInstallLogSink(LogSinkFn fn);
```
`src/engine/src/core/EngineLogSink.cpp`:
```cpp
#include "EngineLogSink.h"
void EngineInstallLogSink(LogSinkFn fn) { sm_set_log_sink(fn); }   // sets Engine.dll's copy
```
Add `src/core/EngineLogSink.cpp` to the engine source list in `src/engine/CMakeLists.txt`.

- [ ] **Step 2: ecs installer**

`src/common/include/EcsLogSink.h`:
```cpp
#pragma once
#include "ECS.h"   // ECS_API
#include "lib.h"   // LogSinkFn
ECS_API void EcsInstallLogSink(LogSinkFn fn);
```
`src/ecs/src/EcsLogSink.cpp`:
```cpp
#include "EcsLogSink.h"
void EcsInstallLogSink(LogSinkFn fn) { sm_set_log_sink(fn); }   // sets ecs.dll's copy
```
Add `src/EcsLogSink.cpp` to the ecs source list in `src/ecs/CMakeLists.txt`.

> Engineer note: confirm the exact relative path style used in `src/ecs/CMakeLists.txt` (it lists `.cpp`s explicitly — match the existing entries' path form). Put `EcsLogSink.cpp` under `src/ecs/src/` next to the other ecs sources; create the `src/` subdir entry to match siblings.

- [ ] **Step 3: netlib installer**

`src/netlib/include/netlib/log_sink.h`:
```cpp
#pragma once
#include "netlib/netlib_api.h"   // NETLIB_API
#include "lib.h"                 // LogSinkFn (common/include is on netlib's path)
namespace netlib { NETLIB_API void SetLogSink(LogSinkFn fn); }
```
`src/netlib/src/LogSink.cpp`:
```cpp
#include "netlib/log_sink.h"
namespace netlib { void SetLogSink(LogSinkFn fn) { ::sm_set_log_sink(fn); } }   // sets netlib.dll's copy
```
Add `src/LogSink.cpp` to the netlib source list in `src/netlib/CMakeLists.txt`.

> Engineer note: netlib already has `../common/include` on its PRIVATE include path and uses `SM_*` macros, so `lib.h` resolves. The public header `netlib/log_sink.h` is consumed by the editor, which also has `common/include` — so its `#include "lib.h"` resolves there too.

- [ ] **Step 4: Game installer** — add to `src/game/src/game.cpp` (near the other `extern "C"` exports / `GameRegisterSystems`)

```cpp
extern "C" EXPORT_FN void GameInstallLogSink(LogSinkFn fn) { sm_set_log_sink(fn); }
```
(`EXPORT_FN` and `LogSinkFn`/`sm_set_log_sink` come from `lib.h`, already included transitively in `game.cpp`. No `Game.h`/`GAME_API_VERSION` change — it's resolved by name via `GetProcAddress`, not part of the `GameState` ABI.)

- [ ] **Step 5: GameLibrary forwards the sink after load** — in `src/engine/src/threading/GameLibrary.cpp`, inside `LoadOrReload`, right after the existing `SM_TRACE("GameLibrary: loaded ...")` (line ~105, after `m_pGameRegisterSystems` is assigned):

```cpp
    // Forward Engine.dll's installed log sink into the freshly-loaded Game.dll so its logs
    // reach the editor console too (and survive hot-reload). Null in runtime/server → game stdout-only.
    if (auto pInstallLog = reinterpret_cast<void(*)(LogSinkFn)>(
            GetProcAddress(newModule, "GameInstallLogSink"))) {
        pInstallLog(g_SmLogSink);
    }
```
(`g_SmLogSink` here is Engine.dll's copy — set by `EngineInstallLogSink` in Task 4. `GameLibrary.cpp` already includes `lib.h` transitively via its headers; if `LogSinkFn` is unresolved, add `#include "lib.h"`.)

- [ ] **Step 6: Build the affected libraries + game, verify clean**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target ecs --target netlib --target Engine --target game
```
Expected: all four build clean; `EngineInstallLogSink`, `EcsInstallLogSink`, `netlib::SetLogSink`, `GameInstallLogSink` all compile/export.

- [ ] **Step 7: Commit**

```bash
git add src/engine/include/EngineLogSink.h src/engine/src/core/EngineLogSink.cpp \
        src/common/include/EcsLogSink.h src/ecs/src/EcsLogSink.cpp \
        src/netlib/include/netlib/log_sink.h src/netlib/src/LogSink.cpp \
        src/game/src/game.cpp src/engine/src/threading/GameLibrary.cpp \
        src/engine/CMakeLists.txt src/ecs/CMakeLists.txt src/netlib/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(log): per-module sink installers + GameLibrary forward to Game.dll"
```

---

## Task 4: ConsolePanel + editor wiring (install at startup, draw the panel)

**Files:**
- Create: `src/editor/src/panels/ConsolePanel.h`, `src/editor/src/panels/ConsolePanel.cpp`
- Modify: `src/editor/src/main.cpp` (install sinks at startup)
- Modify: `src/editor/src/app/ImGuiRenderer.h`, `src/editor/src/app/ImGuiRenderer.cpp`
- Modify: `src/editor/src/panels/MainMenuBar.cpp` (optional toggle)
- Modify: `src/editor/CMakeLists.txt`

- [ ] **Step 1: Create `ConsolePanel.h`**

```cpp
#pragma once
#include <deque>
#include <string>
#include "LogBus.h"   // LogRecord, LogBus

// Editor Console panel: drains LogBus each frame into a bounded scrollback, renders
// filtered + colored. Owned by ImGuiRenderer (RenderThread). One instance.
class ConsolePanel {
public:
    void Draw(bool* open);   // call once per frame

private:
    void DrainNew();         // pull new records from LogBus into m_Lines

    static constexpr size_t kScrollback = 10000;
    std::deque<LogRecord> m_Lines;

    bool  m_ShowTrace = true;
    bool  m_ShowWarn  = true;
    bool  m_ShowError = true;
    bool  m_AutoScroll = true;
    char  m_Filter[128] = {0};
};
```

- [ ] **Step 2: Create `ConsolePanel.cpp`**

```cpp
#include "ConsolePanel.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace {
    ImVec4 LevelColor(LogLevel l) {
        switch (l) {
            case LogLevel::Warn:  return ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
            case LogLevel::Error: return ImVec4(1.0f, 0.4f,  0.4f, 1.0f);
            default:              return ImVec4(0.7f, 0.85f, 0.7f, 1.0f); // Trace
        }
    }
    bool ContainsCI(const char* hay, const char* needle) {
        if (!needle || !needle[0]) return true;
        std::string h(hay), n(needle);
        auto lower = [](std::string& s){ for (char& c : s) c = (char)std::tolower((unsigned char)c); };
        lower(h); lower(n);
        return h.find(n) != std::string::npos;
    }
}

void ConsolePanel::DrainNew() {
    LogRecord buf[256];
    size_t n;
    do {
        n = LogBus::Instance().Drain(buf, 256);
        for (size_t i = 0; i < n; ++i) {
            if (m_Lines.size() >= kScrollback) m_Lines.pop_front();
            m_Lines.push_back(buf[i]);
        }
    } while (n == 256);
}

void ConsolePanel::Draw(bool* open) {
    DrainNew();   // drain every frame even when hidden, so the ring never overflows from neglect

    if (open && !*open) return;
    if (!ImGui::Begin("Console", open)) { ImGui::End(); return; }

    ImGui::Checkbox("Trace", &m_ShowTrace); ImGui::SameLine();
    ImGui::Checkbox("Warn",  &m_ShowWarn);  ImGui::SameLine();
    ImGui::Checkbox("Error", &m_ShowError); ImGui::SameLine();
    ImGui::Checkbox("Autoscroll", &m_AutoScroll); ImGui::SameLine();
    if (ImGui::Button("Clear")) m_Lines.clear(); ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##filter", "filter...", m_Filter, sizeof(m_Filter));

    const uint64_t dropped = LogBus::Instance().DroppedCount();
    if (dropped > 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1,0.5f,0.2f,1), "(%llu dropped)", (unsigned long long)dropped);
    }

    ImGui::Separator();
    ImGui::BeginChild("##log", ImVec2(0,0), false, ImGuiWindowFlags_HorizontalScrollbar);

    auto visible = [&](const LogRecord& r) {
        if (r.level == LogLevel::Trace && !m_ShowTrace) return false;
        if (r.level == LogLevel::Warn  && !m_ShowWarn)  return false;
        if (r.level == LogLevel::Error && !m_ShowError) return false;
        return ContainsCI(r.text, m_Filter);
    };

    for (const LogRecord& r : m_Lines) {
        if (!visible(r)) continue;
        ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(r.level));
        ImGui::TextUnformatted(r.text);
        ImGui::PopStyleColor();
    }

    if (m_AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::End();
}
```

> Engineer note: this renders every visible line each frame. If profiling later shows it's hot with a full 10k buffer + no filter, swap the loop for an `ImGuiListClipper` over a pre-filtered index vector — deferred (YAGNI) since the buffer is capped and lines are short. Keep the per-frame `DrainNew()` BEFORE the `*open` early-return so a hidden panel still drains the ring.

- [ ] **Step 3: Install sinks at editor startup** — in `src/editor/src/main.cpp`, add includes and install as the FIRST thing in `main()` (before `app.Init`, so startup logs from all modules are captured):

Includes (near the other `#include`s):
```cpp
#include "LogBus.h"
#include "EngineLogSink.h"
#include "EcsLogSink.h"
#include "netlib/log_sink.h"
```
At the very top of `main()`:
```cpp
    // Route every in-process module's logs into the editor console (LogBus). Each DLL has its
    // own header-inlined sink pointer, so each gets installed explicitly. Game.dll is wired by
    // GameLibrary after it loads. Do this first so startup logs are captured.
    sm_set_log_sink(&ConsoleLogSink);     // editor.exe's own copy
    EngineInstallLogSink(&ConsoleLogSink);
    EcsInstallLogSink(&ConsoleLogSink);
    netlib::SetLogSink(&ConsoleLogSink);
```

> Engineer note: confirm `src/editor/src/main.cpp` has `src/editor/src/app` and `src/editor/src/panels` on its include path (the editor target adds `src/app`/`src/panels` — verify in `src/editor/CMakeLists.txt`; `LogBus.h` is under `src/app`). `EngineLogSink.h` resolves via Engine's PUBLIC include (`src/engine/include`); `EcsLogSink.h` via `common/include`; `netlib/log_sink.h` via netlib's include dir.

- [ ] **Step 4: Own + draw the panel in `ImGuiRenderer`**

`src/editor/src/app/ImGuiRenderer.h`: add include + member (next to `m_ServerSupervisor`):
```cpp
#include "ConsolePanel.h"
```
```cpp
    ConsolePanel m_ConsolePanel;
```
`src/editor/src/app/ImGuiRenderer.cpp`: add include at top:
```cpp
#include "ConsolePanel.h"
```
Near the other panel draw calls (where `DrawDedicatedServerPanel(...)` is):
```cpp
        static bool s_ShowConsolePanel = true;
        m_ConsolePanel.Draw(&s_ShowConsolePanel);
```

- [ ] **Step 5 (optional): MainMenuBar toggle** — if `MainMenuBar.cpp` has a simple Windows/View menu listing panels, add a `MenuItem("Console", ...)` toggling `s_ShowConsolePanel` via the existing result/flag mechanism. If the pattern is non-trivial, SKIP (panel defaults visible). State the choice in the report.

- [ ] **Step 6: Add panel source to editor build** — in `src/editor/CMakeLists.txt`, add under "Editor panels":
```cmake
    src/panels/ConsolePanel.cpp
```
And under "App / orchestration layer":
```cmake
    src/app/LogBus.cpp
```

- [ ] **Step 7: Build the editor, verify clean**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: `editor.exe` builds clean.

- [ ] **Step 8: Commit**

```bash
git add src/editor/src/panels/ConsolePanel.h src/editor/src/panels/ConsolePanel.cpp \
        src/editor/src/main.cpp src/editor/src/app/ImGuiRenderer.h src/editor/src/app/ImGuiRenderer.cpp \
        src/editor/src/panels/MainMenuBar.cpp src/editor/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(log): editor Console panel + startup sink install across modules"
```

---

## Task 5: Full rebuild, regression, manual smoke, docs

**Files:**
- Modify: `docs/superpowers/specs/2026-05-30-in-editor-log-console-design.md` (status)

- [ ] **Step 1: Full rebuild + run the relevant test suite**

Run:
```
cmake --build --preset msvc-win64-vs2026-community
./out/build/msvc-win64-vs2026-community/bin/Debug/test_logformat.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_logbus.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_net.exe
```
Expected: all green (`All log format tests passed.`, `All logbus tests passed.`, `All ECS tests passed.`, `All net tests passed.`). A full all-targets build must succeed.

- [ ] **Step 2: Manual smoke (USER-owned — interactive GUI)**

Launch `editor.exe`. Confirm the **Console** panel shows live logs: an editor action (e.g. open a panel), a `Game.dll` tick log, a `netlib` connect (Dedicated Server panel → Start), an engine nav rebuild. Verify: correct per-level colors; Trace/Warn/Error filter checkboxes hide/show; text filter narrows; autoscroll sticks to bottom; Clear empties; the console (terminal) still prints everything (tee preserved). Flood logs and confirm the "(N dropped)" indicator appears without a crash. Hot-reload `Game.dll` (rebuild `game`) and confirm its logs still reach the panel.

- [ ] **Step 3: Mark the spec done**

In `docs/superpowers/specs/2026-05-30-in-editor-log-console-design.md`, change `Status:` to implemented + add a one-line verification note (automated tests green; manual smoke confirmed).

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-05-30-in-editor-log-console-design.md
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "docs(log): mark in-editor console implemented + verification note"
```

---

## Self-Review

**Spec coverage:**
- Capture all in-process modules → Task 1 (hook) + Task 3 (per-module installers + Game forward) + Task 4 (editor install). ✓
- Structured sink (level/color/plain text), stdout preserved → Task 1. ✓
- `LogBus` lock-free ring, trivially-copyable record, drop-count, truncation → Task 2. ✓
- ConsolePanel: level filters, text filter, autoscroll, clear, color, dropped indicator, dock/menu → Task 4. ✓
- Hot-reload re-install → Task 3 (GameLibrary forward). ✓
- runtime/server unaffected → Task 1 (null sink) + Task 3 (Game forward forwards null) — no installer called in those exes. ✓
- Testing: `format_log_body`/sink-hook (Task 1), `LogBus` round-trip/truncate/overflow (Task 2), manual smoke (Task 5). ✓
- Defaults (kLogTextMax 240, scrollback 10k, ring 8192, drop-newest) → Tasks 2/4. ✓

**Placeholder scan:** no TBD/TODO; every code step shows complete code; engineer-notes only ask to confirm existing path/method conventions, not defer design.

**Type consistency:** `LogSinkFn = void(*)(LogLevel, TextColor, const char*)` identical across lib.h, all four installers, `ConsoleLogSink`, GameLibrary's `GetProcAddress` cast. `LogRecord` fields used identically in `LogBus.cpp`, the test, and `ConsolePanel.cpp`. `LogBus::Push/Drain/DroppedCount` signatures match between header, `.cpp`, test, and panel. `kLogTextMax` used consistently (header constant). `format_log_body(prefix, msg, args...)` matches its caller in `_log`.

**Known restart point:** Task 1 edits `lib.h` (included everywhere) → Task 5's full rebuild + an editor restart are required before the panel reflects the new code. Flagged in the plan header.
