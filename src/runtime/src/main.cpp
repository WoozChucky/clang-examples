#include <algorithm>
#include <GLFW/glfw3.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <mutex>
#include <filesystem>
#include <windows.h>

#include "lib.h"
#include "Application.h"
#include "utilities/SettingsManager.h"

// Per-module SM_ASSERT backend (platform_debug_break is declared non-exported in lib.h,
// so each executable provides its own definition; mirrors editor.exe / Engine.dll).
void platform_debug_break(const char* expr, const char* file, int line, const char* message)
{
    char buffer[2048] = {};
    sprintf_s(buffer,
            "Assertion failed!\n\nExpression: %s\nFile: %s\nLine: %d\n\n%s",
            (expr ? expr : "<none>"),
            (file ? file : "<unknown>"),
            line,
            (message ? message : "<no message>"));

    MessageBoxA(nullptr, buffer, "Assertion Failed", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);

    DEBUG_BREAK();
}

namespace {
    void PrintUsage() {
        std::printf("Usage: runtime.exe [--backend=<api>] [--help]\n"
                    "\n"
                    "  --backend=<api>   Override the persisted renderer backend for this run\n"
                    "                    only. Does NOT modify editor_settings.json.\n"
                    "                    Valid values: vulkan, vk, directx12, dx12.\n"
                    "                    (directx11/dx11 is reserved but the backend is not\n"
                    "                    implemented yet.)\n"
                    "  --help, -h        Print this help and exit.\n");
    }

    struct CliResult {
        std::optional<RendererAPI> override_;
        bool parseOk = true;
        bool helpRequested = false;
    };

    CliResult ParseCli(int argc, char** argv) {
        CliResult r;
        for (int i = 1; i < argc; ++i) {
            std::string_view arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                r.helpRequested = true;
                continue;
            }
            constexpr std::string_view kBackend = "--backend=";
            if (arg.substr(0, kBackend.size()) == kBackend) {
                const std::string_view value = arg.substr(kBackend.size());
                const RendererAPI parsed = SettingsManager::ParseBackend(value);
                if (parsed == RendererAPI::Invalid) {
                    std::printf("Error: invalid --backend value '%.*s'.\n\n",
                                static_cast<int>(value.size()), value.data());
                    r.parseOk = false;
                    return r;
                }
                r.override_ = parsed; // last occurrence wins
                continue;
            }
            std::printf("Error: unknown argument '%.*s'.\n\n",
                        static_cast<int>(arg.size()), arg.data());
            r.parseOk = false;
            return r;
        }
        return r;
    }
}

int main(int argc, char** argv) {
    SM_TRACE("Working Directory: %s", std::filesystem::current_path().string().c_str());

    const CliResult cli = ParseCli(argc, argv);
    if (!cli.parseOk) {
        PrintUsage();
        return 1;
    }
    if (cli.helpRequested) {
        PrintUsage();
        return 0;
    }

    Application app;
    if (!app.Init(cli.override_)) {   // no overlay factory -> no ImGui
        SM_ERROR("Application initialization failed!");
        MessageBoxA(nullptr,
                    "Renderer initialization failed.\n\n"
                    "Edit editor_settings.json next to runtime.exe, or relaunch with\n"
                    "  --backend=vulkan\n"
                    "  --backend=directx12\n",
                    "Runtime — startup failure",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return -1;
    }

    app.Run();

    SM_TRACE("Shutdown complete.");
    return 0;
}
