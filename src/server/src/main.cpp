#include <windows.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "lib.h"
#include "core/ServerApplication.h"
#include "ServerControl.h"

void platform_debug_break(const char* expr, const char* file, int line, const char* message) {
    char buffer[2048] = {};
    sprintf_s(buffer, "Assertion failed!\n\nExpression: %s\nFile: %s\nLine: %d\n\n%s",
              (expr ? expr : "<none>"), (file ? file : "<unknown>"), line,
              (message ? message : "<no message>"));
    SM_ERROR("%s", buffer);
    DEBUG_BREAK();
}

namespace {
    ServerApplication* g_App = nullptr;

    BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
        if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT ||
            type == CTRL_BREAK_EVENT || type == CTRL_SHUTDOWN_EVENT) {
            if (g_App) g_App->RequestShutdown();
            return TRUE;
        }
        return FALSE;
    }

    struct Cli { uint16_t port = kDedicatedServerDefaultPort; std::string world; bool ok = true; };

    Cli ParseCli(int argc, char** argv) {
        Cli c;
        for (int i = 1; i < argc; ++i) {
            std::string_view a = argv[i];
            constexpr std::string_view kPort = "--port=";
            constexpr std::string_view kWorld = "--world=";
            if (a.substr(0, kPort.size()) == kPort) {
                const std::string val(a.substr(kPort.size()));
                int parsed = 0;
                try { parsed = std::stoi(val); } catch (const std::exception&) { parsed = -1; }
                if (parsed < 1 || parsed > 65535) {
                    std::printf("server.exe: invalid --port value '%s' (expected 1-65535)\n", val.c_str());
                    c.ok = false; return c;
                }
                c.port = static_cast<uint16_t>(parsed);
            } else if (a.substr(0, kWorld.size()) == kWorld) {
                c.world = std::string(a.substr(kWorld.size()));
            } else {
                std::printf("server.exe: unknown arg '%.*s'\n", (int)a.size(), a.data());
                c.ok = false; return c;
            }
        }
        return c;
    }
}

int main(int argc, char** argv) {
    SM_TRACE("server.exe working dir: %s", std::filesystem::current_path().string().c_str());
    const Cli cli = ParseCli(argc, argv);
    if (!cli.ok) {
        std::printf("Usage: server.exe [--port=<n>] [--world=<path>]\n");
        return 1;
    }

    ServerApplication app;
    g_App = &app;
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    ServerApplication::Config cfg;
    cfg.port = cli.port;
    cfg.worldPath = cli.world;
    if (!app.Init(cfg)) {
        SM_ERROR("server.exe: ServerApplication init failed");
        return -1;
    }

    const std::string evName = ServerShutdownEventName(cli.port);
    HANDLE shutdownEvent = CreateEventA(nullptr, TRUE, FALSE, evName.c_str());
    HANDLE waiter = nullptr;
    if (shutdownEvent) {
        waiter = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
            HANDLE ev = static_cast<HANDLE>(p);
            WaitForSingleObject(ev, INFINITE);
            if (g_App) g_App->RequestShutdown();
            return 0;
        }, shutdownEvent, 0, nullptr);
    } else {
        SM_WARN("server.exe: could not create shutdown event '%s' (err %lu); Ctrl-C only",
                evName.c_str(), GetLastError());
    }

    app.Run();
    app.Shutdown();
    g_App = nullptr;

    if (shutdownEvent) {
        SetEvent(shutdownEvent);
        if (waiter) { WaitForSingleObject(waiter, 2000); CloseHandle(waiter); }
        CloseHandle(shutdownEvent);
    }
    SM_TRACE("server.exe: clean exit");
    return 0;
}
