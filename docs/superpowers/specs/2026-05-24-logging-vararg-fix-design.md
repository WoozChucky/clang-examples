# Logging Vararg Quirk Fix (harden `_log`) — Design

**Date:** 2026-05-24
**Status:** Approved (design); pending implementation plan.
**Branch:** stacking on `main` (implementation on `logging-vararg-fix`).

## Goal

Fix the long-standing quirks in the `SM_TRACE`/`SM_WARN`/`SM_ERROR` logging path (`src/common/include/lib.h`)
while keeping the printf-style (`%`) API so **all ~299 call sites stay unchanged** — they simply start
printing correct output. Extract the formatting into a small, pure, unit-tested header.

## Background (verified, `src/common/include/lib.h`)

```cpp
enum TextColor { TEXT_COLOR_BLACK, TEXT_COLOR_RED, ... TEXT_COLOR_COUNT };  // :84-103

template <typename... Args>
void _log(const char* prefix, const char* msg, TextColor textColor, Args... args) {  // :105
    static const char* TextColorTable[TEXT_COLOR_COUNT] = { "\x1b[30m", ... };        // :108-126
    char formatBuffer[8192] = {};
    sprintf(formatBuffer, "%s %s %s \033[0m", TextColorTable[textColor], prefix, msg);// :129
    static char buffer[8192] = {};
    sprintf(buffer, formatBuffer, args...);                                            // :132
    std::println("{}", buffer);                                                        // :133
}
#define SM_TRACE(msg, ...) do { _log("TRACE:", msg, TEXT_COLOR_GREEN, ##__VA_ARGS__); } while(0)        // :136
#define SM_WARN(msg, ...)  do { _log("WARN:",  msg, TEXT_COLOR_YELLOW, "\033[0m", ##__VA_ARGS__); } while(0) // :137
#define SM_ERROR(msg, ...) do { _log("ERROR:", msg, TEXT_COLOR_RED,    "\033[0m", ##__VA_ARGS__); } while(0) // :138

template <typename... Args>
inline void sm_assert_fail(..., const char* fmt, Args... args) {                       // :144
    char formatted[1024] = {};
    sprintf(formatted, fmt, args...);                                                  // :149
    _log("ERROR:", "%s", TEXT_COLOR_RED, "\033[0m", formatted);                        // :151
    platform_debug_break(expr, file, line, formatted);
}
```

**Confirmed defects:**
1. **Off-by-one in `SM_WARN`/`SM_ERROR`.** They pass an extra `"\033[0m"` as the first vararg. The
   reset is *already* in the format literal (`... \033[0m` at :129), so this extra string is not a
   second printed reset — it lands in `args...` and is consumed by the user message's **first `%`
   specifier**, shifting every format argument by one. `SM_ERROR("req %zu", n)` prints the escape
   string for `%zu` and drops `n`. (`SM_TRACE` has no extra arg, so it formats correctly.) Likewise
   `sm_assert_fail` passes the extra `"\033[0m"` (:151), so assert messages with format args are
   shifted. **This is the "vararg quirk" the snprintf-preformat workaround dodges.**
2. **Unbounded `sprintf`** (:129, :132, :149) → buffer overflow if output exceeds the fixed sizes.
3. **`static char buffer[8192]`** (:131) is shared across all three threads (Platform/Game/Render all
   log) → data race / interleaved-garbled output / UB.
4. **Msg-as-format-string fragility** — the user `msg` is baked into `formatBuffer` and then used as
   the *format string* of the second `sprintf`, so a `%` inside runtime `msg` data is misinterpreted.

**Inherent (not fixable without leaving printf):** passing non-POD args (e.g. `std::string` without
`.c_str()`) to a C-variadic is UB — the documented convention (`.c_str()`) remains.

**Call sites:** ~299 across 33 files; many `SM_WARN`/`SM_ERROR` with format args are currently wrong
(e.g. `ArenaAllocator.h:55`, `GameThread.cpp:290/313`, `Renderer.cpp:163`, the Vulkan backend). All
keep their `%`-style strings — no call-site edits.

## Scope

**In scope:**
1. New `src/common/include/LogFormat.h` — pure + testable: the `TextColor` enum, `text_color_code()`,
   and a `format_log_line(...)` that formats the message+args ONCE and returns the fully-wrapped line
   (color + prefix + body + reset) as a `std::string`.
2. Rewire `lib.h`: include `LogFormat.h`; `_log` becomes `std::println("{}", format_log_line(...))`;
   drop the redundant `"\033[0m"` vararg from `SM_WARN`/`SM_ERROR`; fix `sm_assert_fail`
   (`snprintf` + drop the extra reset arg). Remove the now-duplicated enum/table from `lib.h`.
3. `test_logformat` unit test proving correct substitution (the off-by-one fix) + the reset suffix.

**Out of scope / non-goals:** migrating call sites to `std::format` (`{}`-style) — too large/risky for
299 sites; changing the log API/signature; touching `SM_ASSERT`/`platform_debug_break` behavior
(only `sm_assert_fail`'s formatting); escaping `%` in existing literal messages (pre-existing printf
convention — `%%`); fixing non-POD-arg UB beyond keeping the `.c_str()` convention. No
`GAME_API_VERSION` bump (no `GameState`/export/component change).

## Design

### 1. `LogFormat.h` (pure, unit-tested)

```cpp
#pragma once
#include <cstdio>   // std::snprintf
#include <string>

enum TextColor {
    TEXT_COLOR_BLACK, TEXT_COLOR_RED, TEXT_COLOR_GREEN, TEXT_COLOR_YELLOW,
    TEXT_COLOR_BLUE, TEXT_COLOR_MAGENTA, TEXT_COLOR_CYAN, TEXT_COLOR_WHITE,
    TEXT_COLOR_BRIGHT_BLACK, TEXT_COLOR_BRIGHT_RED, TEXT_COLOR_BRIGHT_GREEN,
    TEXT_COLOR_BRIGHT_YELLOW, TEXT_COLOR_BRIGHT_BLUE, TEXT_COLOR_BRIGHT_MAGENTA,
    TEXT_COLOR_BRIGHT_CYAN, TEXT_COLOR_BRIGHT_WHITE, TEXT_COLOR_COUNT
};

inline const char* text_color_code(TextColor c) {
    static const char* kTable[TEXT_COLOR_COUNT] = {
        "\x1b[30m","\x1b[31m","\x1b[32m","\x1b[33m","\x1b[34m","\x1b[35m","\x1b[36m","\x1b[37m",
        "\x1b[90m","\x1b[91m","\x1b[92m","\x1b[93m","\x1b[94m","\x1b[95m","\x1b[96m","\x1b[97m",
    };
    return (c >= 0 && c < TEXT_COLOR_COUNT) ? kTable[c] : "";
}

// Formats the user message + args ONCE (printf-style), then wraps it: "<color> <prefix> <body>\033[0m".
// The reset (\033[0m) is part of the literal here — never a passed argument.
template <typename... Args>
std::string format_log_line(const char* prefix, const char* msg, TextColor color, Args... args) {
    char body[8192];
    std::snprintf(body, sizeof(body), msg, args...);
    std::string out;
    out += text_color_code(color);
    out += ' ';
    out += prefix;
    out += ' ';
    out += body;
    out += "\033[0m";
    return out;
}
```
- Param order mirrors the existing `_log` (`prefix, msg, color, args...`) so the macros are unchanged
  except for dropping the extra reset arg.
- `text_color_code` is `inline` (single definition across TUs). Bounds-guarded.
- Output layout is exactly `"<code> <prefix> <body>\033[0m"` — single space after the color code and
  after the prefix, body, then the reset with **no** space before it. (Today's `"%s %s %s \033[0m"`
  had a stray space before the reset; dropping it is an intentional, invisible cleanup.) The
  `test_logformat` strings pin this exactly.

### 2. `lib.h` rewire

Replace the logging section (the `enum TextColor` through the three `SM_*` macros, lines ~84-138)
with `#include "LogFormat.h"` plus the slimmed `_log` + macros:
```cpp
#include "LogFormat.h"

template <typename... Args>
void _log(const char* prefix, const char* msg, TextColor color, Args... args) {
    std::println("{}", format_log_line(prefix, msg, color, args...));
}

#define SM_TRACE(msg, ...) do { _log("TRACE:", msg, TEXT_COLOR_GREEN, ##__VA_ARGS__); } while(0)
#define SM_WARN(msg, ...)  do { _log("WARN:",  msg, TEXT_COLOR_YELLOW, ##__VA_ARGS__); } while(0)
#define SM_ERROR(msg, ...) do { _log("ERROR:", msg, TEXT_COLOR_RED,    ##__VA_ARGS__); } while(0)
```
(`<print>` is already included in `lib.h`. The `enum TextColor` + `TextColorTable` are deleted from
`lib.h` — now provided by `LogFormat.h`.)

`sm_assert_fail` (stays in `lib.h`): `sprintf`→`snprintf`, and drop the extra `"\033[0m"`:
```cpp
char formatted[1024] = {};
std::snprintf(formatted, sizeof(formatted), fmt, args...);
_log("ERROR:", "%s", TEXT_COLOR_RED, formatted);   // was: ..., "\033[0m", formatted
platform_debug_break(expr, file, line, formatted);
```

### 3. Behavior change (intended)

`SM_WARN`/`SM_ERROR`/assert messages **with format args** go from garbled/shifted to **correct**.
`SM_TRACE` is unchanged in behavior. Preformatted-string call sites (`SM_WARN(somePreformattedBuf)`
with no `%` and no args) are unchanged. Color/reset output is unchanged (the reset stays in the
literal). No call-site edits.

## Build / verification

Build preset `msvc-win64-vs2026-community`. `lib.h` is included extremely widely → rebuild
engine/editor/runtime/game/all tests. No `GAME_API_VERSION` bump.

- **Unit test (`test_logformat`)** — pure assertions on `format_log_line` (this is the proof the
  off-by-one is fixed):
  - `format_log_line("TRACE:", "hello", TEXT_COLOR_GREEN)` → `"\x1b[32m TRACE: hello\033[0m"`.
  - `format_log_line("WARN:", "n=%d", TEXT_COLOR_YELLOW, 5)` → `"\x1b[33m WARN: n=5\033[0m"` (args
    not shifted).
  - `format_log_line("ERROR:", "%s=%zu", TEXT_COLOR_RED, "x", size_t{42})` →
    `"\x1b[31m ERROR: x=42\033[0m"` (multiple, mixed-type args in order).
  - A no-arg message with a literal containing no `%` round-trips unchanged.
  Prints `All log format tests passed.`
- Full build clean (every TU that includes `lib.h` recompiles — the real validation that nothing
  broke). `test_ecs`/`test_alloc`/`test_frustum`/`test_input`/`test_picking`/`test_editorcam`/
  `test_metrichistory`/`test_transientstatus` stay green.
- **Console smoke (observe, optional):** the intentional `test_alloc` `ArenaAllocator '...' exhausted:
  req .. used .. cap ..` line now shows the real numbers instead of garbage; editor/runtime logs with
  `%`-args render correctly.

## Risks

- **`-Wformat-nonliteral`/`-Wformat-security`** on `std::snprintf(body, sizeof(body), msg, args...)`
  (non-literal format) — the current code already passes a non-literal format to `sprintf`, so the
  project's warning settings tolerate it; confirm no new warnings-as-errors. (If a toolchain promotes
  it, suppress locally around the one call, matching the existing posture — do not change call sites.)
- **Wide recompile** — `lib.h`/`LogFormat.h` touch everything; mitigated by the full-build gate.
- **Exact spacing of the wrapped line** — pinned by `test_logformat`; keep `_log`'s output identical
  to `format_log_line`'s return (just `println` it).
- **Non-POD args remain UB** — unchanged convention (`.c_str()`); out of scope to fix here.
