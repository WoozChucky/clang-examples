#include <algorithm>
#include <GLFW/glfw3.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <mutex>
#include <filesystem>

#include "alloc.h"
#include "lib.h"
#include "Application.h"

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

int main() {
    std::atexit([](){ DumpAllocations(); });

	SM_TRACE("Working Directory: %s", std::filesystem::current_path().string().c_str());

    Application app;
    if (!app.Init()) {
        SM_ERROR("Application initialization failed!");
        return -1;
    }

    app.Run();

    SM_TRACE("Shutdown complete.");
    return 0;
}
