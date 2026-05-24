# Logging Vararg Quirk Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the `SM_TRACE`/`SM_WARN`/`SM_ERROR` printf quirks (off-by-one reset arg, unbounded `sprintf`, shared `static` buffer race) by extracting a pure, testable formatter and rewiring `_log` — leaving all ~299 call sites unchanged.

**Architecture:** A new header-only `LogFormat.h` holds the `TextColor` enum, `text_color_code()`, and a pure `format_log_line()` that formats the message+args once and returns the wrapped line. `_log` becomes `println(format_log_line(...))`; the redundant `"\033[0m"` vararg is dropped from `SM_WARN`/`SM_ERROR`/`sm_assert_fail`. `%`-style call sites are untouched.

**Tech Stack:** C++23, printf-style formatting via `std::snprintf`, `std::println`, CMake presets.

**Spec:** `docs/superpowers/specs/2026-05-24-logging-vararg-fix-design.md`

**Conventions (every task):**
- Build preset `msvc-win64-vs2026-community`; build dir `out/build/msvc-win64-vs2026-community`; exes in `bin/Debug/`.
- **No `GAME_API_VERSION` bump** (no `GameState`/export/component change). `lib.h`/`LogFormat.h` are included extremely widely → Task 2 recompiles engine/editor/runtime/game/all tests.
- Commit author repo default (`Nuno Silva <nuno.levezinho@live.com.pt>`) — plain `git commit`, no `-c`/`--author`. Never stage `.claude/`. Stage only the files each step names. After each commit, `git log -1 --format='%an <%ae>'` must show the personal email.
- New files/targets require a CMake reconfigure (`cmake --preset msvc-win64-vs2026-community`) before building.

---

### Task 1: `LogFormat.h` + `test_logformat` (TDD)

**Files:**
- Create: `src/common/include/LogFormat.h` (header-only)
- Create: `tests/test_logformat.cpp`
- Modify: `tests/CMakeLists.txt`

`LogFormat.h` is new and not yet included by `lib.h` (Task 2 wires it in), so it cannot clash with
`lib.h`'s existing `enum TextColor` (no TU includes both until Task 2). TDD: write the test, build
(fail — header missing), implement, build (pass).

- [ ] **Step 1: Write the header**

`src/common/include/LogFormat.h`:
```cpp
#pragma once
#include <cstdio>   // std::snprintf
#include <string>

// ANSI terminal colors for logging. (Moved here from lib.h so the formatter is small + testable.)
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

// Formats the user message + args ONCE (printf-style) into a bounded stack buffer, then wraps it:
// "<color> <prefix> <body>\033[0m". The reset (\033[0m) is part of this literal, never a passed arg
// (that was the SM_WARN/SM_ERROR off-by-one bug). Pure: no globals, no I/O -> unit-testable.
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

- [ ] **Step 2: Write the failing test**

`tests/test_logformat.cpp`:
```cpp
#include <cstdio>
#include <cstddef>
#include <string>

#include "LogFormat.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static void T00_plain_no_args()
{
    EXPECT(format_log_line("TRACE:", "hello", TEXT_COLOR_GREEN)
           == std::string("\x1b[32m TRACE: hello\033[0m"));
}

static void T01_single_int_arg_not_shifted()
{
    // The whole point: the int arg fills %d (it is NOT shifted by a phantom reset arg).
    EXPECT(format_log_line("WARN:", "n=%d", TEXT_COLOR_YELLOW, 5)
           == std::string("\x1b[33m WARN: n=5\033[0m"));
}

static void T02_multiple_mixed_args_in_order()
{
    EXPECT(format_log_line("ERROR:", "%s=%zu", TEXT_COLOR_RED, "x", static_cast<std::size_t>(42))
           == std::string("\x1b[31m ERROR: x=42\033[0m"));
}

static void T03_literal_without_specifiers()
{
    EXPECT(format_log_line("TRACE:", "plain text", TEXT_COLOR_GREEN)
           == std::string("\x1b[32m TRACE: plain text\033[0m"));
}

int main()
{
    T00_plain_no_args();
    T01_single_int_arg_not_shifted();
    T02_multiple_mixed_args_in_order();
    T03_literal_without_specifiers();

    if (g_Failures == 0) { std::printf("All log format tests passed.\n"); return 0; }
    std::printf("%d log format test(s) FAILED.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 3: Wire the `test_logformat` CMake target**

In `tests/CMakeLists.txt`, after the last test block (e.g. `test_transientstatus`), append:
```cmake
add_executable(test_logformat
    test_logformat.cpp
)

target_include_directories(test_logformat PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
)

set_target_properties(test_logformat PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```
(No glm. `LogFormat.h` needs only `<cstdio>`/`<string>` — not `<print>` — so it's light.)

- [ ] **Step 4: Reconfigure + build + run (expect PASS)**

```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target test_logformat
./out/build/msvc-win64-vs2026-community/bin/Debug/test_logformat.exe
```
Expected: `All log format tests passed.` (TDD: to see red, build before writing the header — it fails to find `LogFormat.h`. Final state must be green.) If an assertion fails, fix the HEADER, not the test. If you believe an assertion is genuinely wrong (e.g. spacing), STOP and report rather than weakening it.

- [ ] **Step 5: Commit**

```bash
git add src/common/include/LogFormat.h tests/test_logformat.cpp tests/CMakeLists.txt
git commit -m "feat: add LogFormat.h (pure format_log_line) + test_logformat"
```

---

### Task 2: Rewire `lib.h` to use `LogFormat.h`

**Files:**
- Modify: `src/common/include/lib.h`

No new unit test (Task 1's `test_logformat` is the proof; this task is the wiring + the macro/assert
fixes). The validation is the full rebuild (every TU including `lib.h` recompiles) + the regression
suite staying green.

- [ ] **Step 1: Include `LogFormat.h` + delete the duplicated enum/table**

In `src/common/include/lib.h`, in the `// Logging` section (around lines 80-138):
- Add `#include "LogFormat.h"` (e.g. just under the existing `#include <print>` / `#include <ostream>`).
- DELETE the `enum TextColor { ... TEXT_COLOR_COUNT };` block (now provided by `LogFormat.h`).
- In `_log`, DELETE the `static const char* TextColorTable[TEXT_COLOR_COUNT] = { ... };` table and the
  two `sprintf` + `formatBuffer`/`static char buffer` lines.

- [ ] **Step 2: Replace `_log` body**

Replace the whole `_log` definition with:
```cpp
template <typename... Args>
void _log(const char* prefix, const char* msg, TextColor color, Args... args)
{
    std::println("{}", format_log_line(prefix, msg, color, args...));
}
```
(`<print>` is already included in `lib.h`.)

- [ ] **Step 3: Drop the redundant reset vararg from `SM_WARN`/`SM_ERROR`**

Replace the three macros with (note `SM_WARN`/`SM_ERROR` no longer pass `"\033[0m"`; `SM_TRACE`
is unchanged; keep the `do { } while(0)` form from the earlier semicolon fix):
```cpp
#define SM_TRACE(msg, ...) do { _log("TRACE:", msg, TEXT_COLOR_GREEN, ##__VA_ARGS__); } while(0)
#define SM_WARN(msg, ...)  do { _log("WARN:",  msg, TEXT_COLOR_YELLOW, ##__VA_ARGS__); } while(0)
#define SM_ERROR(msg, ...) do { _log("ERROR:", msg, TEXT_COLOR_RED,    ##__VA_ARGS__); } while(0)
```

- [ ] **Step 4: Fix `sm_assert_fail` (snprintf + drop the reset arg)**

In `sm_assert_fail`, replace:
```cpp
  char formatted[1024] = {};
  sprintf(formatted, fmt, args...);
  // Log through our normal logger
  _log("ERROR:", "%s", TEXT_COLOR_RED, "\033[0m", formatted);
```
with:
```cpp
  char formatted[1024] = {};
  std::snprintf(formatted, sizeof(formatted), fmt, args...);
  // Log through our normal logger
  _log("ERROR:", "%s", TEXT_COLOR_RED, formatted);
```
(Leave the `platform_debug_break(...)` line and `SM_ASSERT` macro unchanged.)

- [ ] **Step 5: Full build + regression (the real validation — lib.h is everywhere)**

```
cmake --build out/build/msvc-win64-vs2026-community --target game
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_alloc test_frustum test_input test_picking test_editorcam test_metrichistory test_transientstatus test_logformat
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_frustum.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_input.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_picking.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_editorcam.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_metrichistory.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_transientstatus.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_logformat.exe
```
Expected: ALL build clean (every consumer of `lib.h` recompiles — this is the validation that no TU broke); each test prints its `All ... passed.` line. **`test_alloc`'s intentional `ERROR: ArenaAllocator '...' exhausted: req .. used .. cap ..` line should now show the REAL numbers** (the off-by-one fix is visible) and `All allocator tests passed.` If the build flags a `-Wformat`/format-nonliteral error on `std::snprintf(..., msg, args...)`, it pre-existed with `sprintf`; suppress locally around that one call matching the project's existing posture (do NOT change call sites) and report it.

- [ ] **Step 6: Commit**

```bash
git add src/common/include/lib.h
git commit -m "fix: _log formats once via format_log_line; drop off-by-one reset vararg (WARN/ERROR/assert now correct)"
```

---

## Final verification (after all tasks)

- [ ] Full build: `cmake --build out/build/msvc-win64-vs2026-community` — all targets green (editor, runtime, game, all test_*).
- [ ] All unit tests print their pass line:
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_frustum.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_input.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_picking.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_editorcam.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_metrichistory.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_transientstatus.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_logformat.exe
```
Expected: `All ... passed.` from each (test_alloc prints the intentional ERROR line — now with correct numbers — before its pass).

- [ ] **Console smoke (user-run; observe):** run `editor.exe` / `runtime.exe` and confirm log lines
  with `%`-format args render correctly (e.g. MeshSystem "Created mesh N with V vertices", SettingsManager
  load line); colors still reset between lines (no bleed); no garbled/interleaved output under the 3
  threads. Nothing in the UI changed — this is a logging-correctness fix.

## Notes / non-goals
- No `GAME_API_VERSION` bump (logging-only).
- `%`-style API preserved — zero call-site edits; non-POD args still need `.c_str()`; literal `%` in a
  no-arg message still needs `%%` (pre-existing printf convention).
- `std::format` migration is explicitly out of scope (299 call sites).
- The `do {} while(0)` macro form (from the earlier semicolon fix) is retained.
