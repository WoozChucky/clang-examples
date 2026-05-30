# In-Editor Log Console — Design Spec

**Date:** 2026-05-30
**Branch:** `feat/log-console`
**Status:** IMPLEMENTED 2026-05-30 (commits `dfaba59`..`66375eb` on `feat/log-console`). Automated tests green (`test_logformat` sink-hook + plain-body, `test_logbus` round-trip/truncate/overflow; full regression suite passes). Structured sink hook + per-module installers (Engine/ecs/netlib/Game-via-GameLibrary) + `LogBus` lock-free ring + `ConsolePanel` all landed. `g_SmLogSink` hardened to `std::atomic`. **Pending: user-owned interactive GUI smoke** (logs from each module appear in the panel; filters/search/autoscroll/clear; flood→dropped-count; Game.dll hot-reload still logs). Deferred (YAGNI, per plan): `ImGuiListClipper`/pre-filtered index for the render loop. MainMenuBar toggle skipped (panel default-visible — no existing per-panel menu plumbing).

## Goal

Show the running application's `SM_TRACE` / `SM_WARN` / `SM_ERROR` logs live inside the editor as a dockable **Console** panel — filterable by level and text, color-coded, autoscrolling — capturing logs from **every in-process module** (editor.exe, Engine.dll, ecs.dll, Game.dll, netlib.dll) and **every thread** (Platform / Game / Render / model-worker / net-adapter), without losing the existing stdout/console output.

## Non-goals (v1)

- The out-of-process dedicated server's logs (`server.exe` is a separate process — its stdout is not captured here; a future follow-up could pipe it in).
- Persisting logs to a file (mentioned as a possible later add; not in v1).
- A per-source-module column (deferred; the record carries enough to add it later).
- Structured/queryable log storage beyond a flat scrollback.

## Background: how logging works today

`SM_TRACE/WARN/ERROR` (defined in `src/common/include/lib.h`) expand to `_log(...)`, a **header-inlined function template** that:
1. formats the message via `format_log_line` (`src/common/include/LogFormat.h`) — which embeds ANSI color codes (`text_color_code(color)` … `\033[0m`), and
2. writes it to stdout via `std::println`.

There is **no central log sink** — every call goes straight to stdout, and because `_log` is header-inlined, **each module (DLL/exe) compiles its own copy**. Logs originate from all modules and all threads. `format_log_line` is pure and already unit-tested (`tests/test_logformat.cpp`).

Key consequence for this feature: the ANSI-formatted string is **not** suitable for the panel (it contains escape codes). The sink must receive the **plain** body plus a level/color, and the panel colorizes via ImGui itself.

## Architecture & data flow

```
SM_TRACE/WARN/ERROR  (any module, any thread)
  → _log: format body ONCE
      ├─ std::println(ANSI line)            // console output unchanged (tee)
      └─ if (g_SmLogSink) g_SmLogSink(level, color, plainBody)
            → LogBus::Push(LogRecord)        // process-global lock-free MpscRing (multi-producer)
ConsolePanel (editor, RenderThread, each frame)
  → drain ring → bounded scrollback deque → filtered + colored ImGui render
```

Single hot-path push, lock-free, no allocation. Reuses the existing `MpscRing` (Vyukov, `src/common/include/MpscRing.h`). Console/stdout output is preserved (important for terminal use, file redirect, and `runtime`/`server`).

### Unit boundaries
- **`lib.h` logging core** — owns the sink hook (`g_SmLogSink`, `sm_set_log_sink`), the `LogLevel` enum, and the split formatter. No knowledge of the ring or the panel.
- **`LogBus`** (new, editor layer) — owns the `MpscRing<LogRecord>` + the free sink function pushed into it + the dropped-record counter. The single bridge between "a log happened" and "the panel can read it." No ImGui.
- **`ConsolePanel`** (new, editor layer) — drains `LogBus`, owns the scrollback deque + filter UI state, renders. No threading logic beyond a per-frame drain.
- **Per-module installers** — one exported function per capturable module that sets *that module's* `g_SmLogSink`.

## Component 1 — `lib.h` logging core changes

In `src/common/include/lib.h` (+ a small split in `LogFormat.h`):

```cpp
// LogFormat.h: new plain-body formatter (no ANSI), alongside the existing format_log_line.
template <typename... Args>
std::string format_log_body(const char* prefix, const char* msg, Args... args) {
    char body[8192];
    std::snprintf(body, sizeof(body), msg, args...);
    std::string out;            // "<prefix> <body>" — no color codes
    out += prefix; out += ' '; out += body;
    return out;
}
```

```cpp
// lib.h:
enum class LogLevel : uint8_t { Trace, Warn, Error };
using LogSinkFn = void(*)(LogLevel level, TextColor color, const char* text);

inline LogSinkFn g_SmLogSink = nullptr;        // ONE copy per module (intended)
inline void sm_set_log_sink(LogSinkFn fn) { g_SmLogSink = fn; }

template <typename... Args>
void _log(LogLevel level, const char* prefix, const char* msg, TextColor color, Args... args) {
    std::println("{}", format_log_line(prefix, msg, color, args...));   // stdout, unchanged (ANSI)
    if (g_SmLogSink) {
        const std::string body = format_log_body(prefix, msg, args...); // plain, no ANSI
        g_SmLogSink(level, color, body.c_str());
    }
}

#define SM_TRACE(msg, ...) do { _log(LogLevel::Trace, "TRACE:", msg, TEXT_COLOR_GREEN,  ##__VA_ARGS__); } while(0)
#define SM_WARN(msg, ...)  do { _log(LogLevel::Warn,  "WARN:",  msg, TEXT_COLOR_YELLOW, ##__VA_ARGS__); } while(0)
#define SM_ERROR(msg, ...) do { _log(LogLevel::Error, "ERROR:", msg, TEXT_COLOR_RED,    ##__VA_ARGS__); } while(0)
```

Notes:
- The double-format (ANSI line for stdout + plain body for the sink) only happens when a sink is installed; sink-less builds (`runtime`/`server`) pay nothing beyond the existing `println`.
- `sm_assert_fail` already calls `_log` — update its call to pass `LogLevel::Error`.
- Adding a parameter to `_log` is an ABI-irrelevant header change (everything inlines), but it touches every TU that logs — a full rebuild of all targets is expected. No `GAME_API_VERSION` bump needed (this is not `GameState`/export-signature change), but rebuild + editor restart is required because `lib.h` is pulled into every module.

## Component 2 — `LogRecord` + `LogBus`

```cpp
// LogBus.h (editor layer, e.g. src/editor/src/app/)
static constexpr int kLogTextMax = 240;

struct LogRecord {                 // trivially-copyable → lives directly in MpscRing (no payload pool)
    LogLevel level;
    uint8_t  color;                // TextColor, for display
    uint32_t seq;                  // monotonic, for stable ordering / dedupe-of-repeats later
    double   t;                    // steady_clock seconds at log time
    char     text[kLogTextMax];    // plain body, truncated + "…" if longer
};

class LogBus {
public:
    static LogBus& Instance();
    // Called from ANY thread (the installed sink). Lock-free push; drops newest + bumps
    // m_Dropped when the ring is full (panel drains every frame, so overflow only under floods).
    void Push(LogLevel level, TextColor color, const char* text);
    // Drains up to N records into out (single consumer = panel/RenderThread). Returns count.
    size_t Drain(LogRecord* out, size_t maxN);
    uint64_t DroppedCount() const;
private:
    MpscRing<LogRecord, 8192> m_Ring;
    std::atomic<uint64_t>     m_Dropped{0};
    std::atomic<uint32_t>     m_Seq{0};
};
```

The installed sink is a free function (`fn-ptr`, no capture) that forwards into the singleton:
```cpp
static void ConsoleLogSink(LogLevel lvl, TextColor col, const char* text) {
    LogBus::Instance().Push(lvl, col, text);
}
```
All modules' `g_SmLogSink` point to this **editor.exe** address — valid for the editor's whole lifetime, including across `Game.dll` reloads. Records cross the DLL boundary by value (trivially copyable). The ring is multi-producer (Vyukov) so concurrent pushes from many threads are safe.

## Component 3 — Cross-DLL install wiring

One exported installer per capturable module; each sets its own module-local `g_SmLogSink`:

| Module | Install entry point | Who calls it |
|---|---|---|
| editor.exe | `sm_set_log_sink(&ConsoleLogSink)` directly | editor startup (ImGuiRenderer/overlay init) |
| Engine.dll | `ENGINE_API void EngineInstallLogSink(LogSinkFn)` | editor startup |
| ecs.dll | `ECS_API void EcsInstallLogSink(LogSinkFn)` | editor startup |
| netlib.dll | `netlib::SetLogSink(LogSinkFn)` (declared in netlib's public header; keeps netlib standalone — it owns the hook, the editor injects) | editor startup |
| Game.dll | `extern "C" EXPORT_FN void GameInstallLogSink(LogSinkFn)` | `GameLibrary::LoadOrReload`, immediately after a successful (re)load |

Each installer body is just `sm_set_log_sink(fn);` compiled into that module. The editor wires all five to `&ConsoleLogSink` at startup; `GameLibrary` re-installs into `Game.dll` after every hot-reload (so a reloaded DLL keeps logging to the panel without per-tick cost).

`runtime.exe` / `server.exe`: install **nothing** → `g_SmLogSink` stays null in every module → stdout-only behavior, byte-for-byte unchanged.

## Component 4 — ConsolePanel (editor, RenderThread)

Each frame: `LogBus::Drain` into a bounded scrollback `std::deque<LogRecord>` (cap ~10,000; oldest evicted). Render with ImGui + `ImGuiListClipper` (so a full 10k buffer stays cheap):

- **Level filters**: Trace / Warn / Error checkboxes, each showing a running count.
- **Text filter**: case-insensitive substring match over `text`.
- **Autoscroll** toggle (stick to bottom; auto-disengages when the user scrolls up).
- **Clear** (clears the display deque; does not touch already-emitted stdout).
- **Copy** (visible/all to clipboard).
- **Per-level color** via `TextColor` → `ImVec4`.
- **Dropped indicator**: if `LogBus::DroppedCount() > 0`, show "N dropped (flood)".
- Docked like the other panels; a **MainMenuBar** toggle (mirror the Navigation/Dedicated Server entries). Owned by `ImGuiRenderer` (value member), drawn in the panel pass.

## Edge cases & behavior

- **Hot-reload**: `g_SmLogSink` in `Game.dll` is re-set by `GameLibrary` after each reload; the sink address (editor.exe) never moves. During the brief window before the first install on initial load, early `Game.dll` logs go to stdout only — acceptable.
- **Threading**: producers are lock-free (`MpscRing`), single consumer is the panel drain on the RenderThread. No locks on the logging hot path.
- **Overflow**: drop-newest + counter (logs are advisory; the panel drains every frame, so overflow is a flood-only event and is surfaced, never silent — matches the project's "log/surface degradation, don't silently skip" convention).
- **Truncation**: messages longer than `kLogTextMax-1` are truncated with a trailing "…"; the full line still went to stdout untruncated.
- **runtime/server**: unaffected (no sink installed).

## Testing

- **Unit** — extend `tests/test_logformat.cpp`: assert `format_log_body` produces the plain `"<prefix> <body>"` with **no** ANSI codes, and that `format_log_line` is unchanged (still ANSI-wrapped). Add a level-mapping check (each macro → expected `LogLevel`).
- **Unit** — new `tests/test_logbus.cpp`: `LogBus::Push`/`Drain` round-trip (records come back in order with correct level/text/seq), truncation of an over-long message, and overflow drop-count (push > ring capacity without draining → `DroppedCount` increments, no crash; mirrors the `test_net` ring tests).
- **Manual smoke** — in the editor, provoke a log from each module: an editor panel edit, a `Game.dll` tick `SM_TRACE`, a `netlib` connect (Dedicated Server panel), an engine nav rebuild. Confirm each appears in the Console with the right level/color; toggle filters + text search; flood with logs and confirm the dropped indicator climbs without a crash; confirm stdout still shows everything.

## Defaults chosen (locked unless changed)

- Game.dll sink wired via `GameLibrary` post-load (not per-tick SystemContext).
- `kLogTextMax = 240` (truncate longer).
- Scrollback cap = 10,000 lines.
- Overflow policy = drop-newest + surfaced counter.
- Ring capacity = 8192 records.

## Build / test note

Build & test with the `msvc-win64-vs2026-community` preset only. Because `lib.h` is included by every module, this change forces a rebuild of all targets and an editor restart. Commit identity: `Nuno Silva <nuno.levezinho@live.com.pt>`.
