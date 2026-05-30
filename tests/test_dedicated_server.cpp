// End-to-end: spawn server.exe, prove it boots headless + listens, then clean-stop.
// NOTE: requires server.exe + Game.dll + assets in CWD (RUNTIME_DIR). The first
// run may surface a Windows Firewall prompt for server.exe — accept it once.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <chrono>

#include "ServerControl.h"

#pragma comment(lib, "ws2_32.lib")

static bool TryConnect(uint16_t port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    const bool ok = (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    ::closesocket(s);
    return ok;
}

int main() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    const uint16_t port = kDedicatedServerDefaultPort;

    std::string cmd = "server.exe --port=" + std::to_string(port);
    std::string cmdMut = cmd;
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL spawned = CreateProcessA(nullptr, cmdMut.data(), nullptr, nullptr, FALSE,
                                        CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    if (!spawned) {
        std::printf("FAIL: CreateProcess(server.exe) err=%lu\n", GetLastError());
        return 1;
    }

    bool connected = false;
    for (int i = 0; i < 150 && !connected; ++i) {
        connected = TryConnect(port);
        if (!connected) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    int rc = 0;
    if (!connected) {
        std::printf("FAIL: could not connect to 127.0.0.1:%u within timeout\n", (unsigned)port);
        rc = 1;
    } else {
        std::printf("OK: server is listening on 127.0.0.1:%u\n", (unsigned)port);
    }

    const std::string evName = ServerShutdownEventName(port);
    HANDLE ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, evName.c_str());
    if (ev) { SetEvent(ev); CloseHandle(ev); }
    else    { std::printf("WARN: OpenEvent('%s') err=%lu; terminating\n", evName.c_str(), GetLastError()); }

    const DWORD wait = WaitForSingleObject(pi.hProcess, 5000);
    if (wait != WAIT_OBJECT_0) {
        std::printf("FAIL: server did not exit cleanly; terminating\n");
        TerminateProcess(pi.hProcess, 1);
        rc = 1;
    } else {
        DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
        std::printf("OK: server exited with code %lu\n", code);
        if (code != 0) rc = 1;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    WSACleanup();

    if (rc == 0) std::printf("All dedicated-server e2e tests passed.\n");
    return rc;
}
