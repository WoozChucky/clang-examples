# Agent Guidelines for clang-examples (for automated agents)

Purpose
- This document tells automated agents (AI assistants, bots) how to safely and productively make changes to the clang-examples repository. It focuses on the `editor` area (where most development happens), build/run/test commands on Windows PowerShell, safe edit boundaries, how to discover related code, minimal testing/smoke checks, commit/PR conventions, and a few concrete examples and rollback steps.

Scope
- Intended audience: AI agents and automated tools collaborating on code+docs in this repo.
- Scope: small/medium code fixes, documentation edits, tests, and low-risk refactors within `src/editor` and adjacent project layers (`src/common`, `src/runtime`). For higher-risk or large changes, the agent should create a clear draft and request human review.

Assumptions (inferred from repo)
- Repo builds with CMake on Windows.
- Primary development platform is Windows (PowerShell). Visual Studio or Ninja are common build runners.
- The editor target lives under `src/editor` with its own `CMakeLists.txt`; other important code is under `src/common`, `src/game`. The `src/runtime`, and `src/overlay` are old projects but may be relevant for old code usage references.

Quick checklist (what the agent will do for a small/editable task)
1. Inspect `src/editor` and related CMake files to find the relevant targets and sources.
2. Make the minimal code/doc change in one or a few files.
3. Run a fast smoke build (CMake preset debug) in PowerShell.
4. Run a minimal runtime smoke test if applicable (launch `main.exe` or other short-running target).
5. Run static checks if present (linters) or basic compile error check (always trough CMake build).

When to stop and ask a human
- The change touches build system files (`CMakeLists.txt`, `CMakePresets.json`) beyond tiny fixes.
- The change modifies third-party vendored code under `third_party/`.
- The change affects cross-cutting systems (memory allocators, platform layer, game API versioning, hot-reload internals).
- A failing build produces complex compile/link errors the agent cannot confidently fix without additional context.
- The change introduces or changes public ABI for the hot-reloaded DLLs (game/overlay) or changes exported symbols.

Safe edit boundaries (rules for automated edits)
- Allowed without human review (low-risk):
  - Fixing typos and adding clarifying comments in code.
  - Small bugfixes with a local compile+smoke-test: non-API logic in `src/editor/src` or `src/common` helpers.
- Require human review (create branch + PR with tests):
  - Changes to `CMakeLists.txt` that alter target linkages, compile flags, or installation paths.
  - Any edits that change the `game` exported interfaces or the `GAME_API_VERSION`.
  - Large refactors that touch many files or change memory/layouts.
  - Adding new third-party dependencies.

How to discover editor code and related context
- Primary locations to inspect:
  - `src/editor/` — editor application code and `CMakeLists.txt` for the editor target.
  - `src/common/` — shared headers and helpers (UI, render, input, sound) used across runtime/editor/game.
  - `src/runtime/` — host executable, platform, and renderer backends (hot-reload behavior lives here). This will be used later down in development after the editor is in a feature complete state.
  - `src/game/` — show what actual game logic is like;
  - `third_party/` — vendored libs (NVRHI, imgui, glm, freetype, etc.).
- Useful quick PowerShell commands (run in repo root) to explore when needed:

```powershell
# List editor source files recursively
Get-ChildItem -Path 'C:\dev\clang-examples\src\editor' -Recurse -File | Select-Object -ExpandProperty FullName

# Print top-level CMake and README
Get-Content -Path 'C:\dev\clang-examples\CMakeLists.txt' -Raw
Get-Content -Path 'C:\dev\clang-examples\README.md' -Raw

# Show build helper
Get-Content -Path 'C:\dev\clang-examples\build.bat' -Raw
```

Build and smoke-test (PowerShell exact commands)
- Standard CMake preset flow (recommended for a full build):

```powershell
# Configure (debug preset)
cmake --preset msvc-win64-vs2026-community

# Build using the same preset
cmake --build --preset msvc-win64-vs2026-community

# Run editor executable (example path produced by presets)
& out/build/msvc-win64-vs2026-community/Debug/editor.exe
```

- Notes:
  - Binaries are placed under `build/<preset>/bin/<Config>/` where `<preset>` is e.g. `debug`, `relwithdebinfo`, etc.
  - The runtime executable target is named `main.exe` (target `runtime`), the game library is `game.dll/game.lib` (target `game`).

Minimal tests and smoke checks to add for small fixes
- If your change is code-level and safe to compile, do the following before committing:
  1. Run the CMake configure + build for `debug` preset.
  2. Exercise the editor briefly: open the window, ensure it doesn't crash instantly, and if your change touches a UI path, toggle panels or press keys (1/2/3/TAB/F5) to exercise those branches, and ESC will close the application.

Examples of acceptable automated edits (with guidance)
1) Bugfix (safe):
   - Before: minor logic error in `src/editor/src/some_file.cpp` that reads uninitialised variable.
   - After: initialize variable; add a short comment and a tiny unit test (if a pure function) under `src/editor/tests/`.
   - Verification: `cmake --preset debug` + `cmake --build --preset debug` passes; runtime starts without crash.

2) Small refactor (safe if limited):
   - Before: duplicated helper code across `src/editor` and `src/common`.
   - After: move helper into `src/common/include/` and update includes. Keep changes minimal, update CMake if needed.
   - Verification: full debug build and smoke test; add a TODO to the PR notes asking human to review scope.

Troubleshooting and rollback
- If a build fails after your change:
  1. Run the failing build command locally to capture the compiler/linker error output.
  2. If errors are obviously related to your edits, revert that file and re-run the build to confirm the revert fixes it.

Key files & directories (map)
- CMakeLists.txt (root) — orchestrates top-level targets and adds subdirectories.
- CMakePresets.json — preconfigured build presets (Debug/RelWithDebInfo/clang toolset).
- assets/ — runtime assets copied to output at build time.
- src/editor/ — editor app (primary editing focus). Contains its own CMakeLists and `src/` sources.
- src/common/ — shared headers & utilities: `ui.h`, `render.h`, `input.h`, `sound.h`.
- src/runtime/ — runtime EXE and platform code (hot-reload logic lives here).
- src/game/ — game logic library (entry points consumed by runtime, statically linked with editor).
- third_party/ — vendored libraries (NVRHI, imgui, glm, freetype, tracy, etc.).
- build/, debug/, relwithdebinfo/, debug-clang/ — build outputs and configurations.

Extra commands for deeper inspection (agent-only)
- To list all C++ source files across the editor target:

```powershell
Get-ChildItem -Path 'C:\dev\clang-examples\src\editor\src' -Recurse -Include *.cpp,*.cxx,*.c,*.h,*.hpp | Select-Object FullName
```

Contact & escalation
- If the agent is unsure, it should for human review and testing.

---
