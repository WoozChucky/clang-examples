#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

enum class ServerStatus { NotStarted, Running, Stopped, Crashed };

// PURE helpers (no Win32 side effects) — unit-tested.
std::string BuildServerArgs(uint16_t port, const std::string& worldPath);
ServerStatus MapExitCodeToStatus(DWORD exitCode);

// Editor-side supervisor for the out-of-process dedicated server.
class ServerSupervisor {
public:
    ServerSupervisor();
    ~ServerSupervisor();

    ServerSupervisor(const ServerSupervisor&) = delete;
    ServerSupervisor& operator=(const ServerSupervisor&) = delete;

    bool Start(uint16_t port, const std::string& worldPath = "", const std::string& exePath = "server.exe");
    void Stop(DWORD cleanWaitMs = 3000);
    ServerStatus Status();
    DWORD        LastExitCode() const { return m_LastExitCode; }
    uint16_t     Port() const { return m_Port; }

private:
    void CloseHandles();

    HANDLE   m_Job     = nullptr;
    HANDLE   m_Process = nullptr;
    HANDLE   m_Thread  = nullptr;
    uint16_t m_Port    = 0;
    DWORD    m_LastExitCode = 0;
    ServerStatus m_Status = ServerStatus::NotStarted;
};
