// Engine-local definition of the SM_ASSERT platform hook.
//
// `platform_debug_break` is a plain forward declaration in lib.h with no
// dllimport/export annotation, so every module that instantiates SM_ASSERT
// needs its own definition. Now that the runtime core lives in Engine.dll,
// the engine TUs reference this symbol and must resolve it locally. The
// editor exe keeps its own copy in main.cpp (separate module, no conflict).
//
// This is the generic Win32 implementation: show a message box, then break.
// It carries no ImGui dependency, so it belongs in the runtime core.

#include "lib.h"

#include <cstdio>

// WIN32_LEAN_AND_MEAN / NOMINMAX are defined globally on the command line.
#include <Windows.h>

void platform_debug_break(const char* expr, const char* file, int line, const char* message)
{
    char buffer[2048] = {};
    // Compose a detailed message
    sprintf_s(buffer,
            "Assertion failed!\n\nExpression: %s\nFile: %s\nLine: %d\n\n%s",
            (expr ? expr : "<none>"),
            (file ? file : "<unknown>"),
            line,
            (message ? message : "<no message>"));

    MessageBoxA(nullptr, buffer, "Assertion Failed", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);

    // Break into the debugger (still platform-defined macro)
    DEBUG_BREAK();
}
