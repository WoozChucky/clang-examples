#include "ServerSupervisor.h"
#include "ServerControl.h"
#include "lib.h"

#include <vector>

std::string BuildServerArgs(uint16_t port, const std::string& worldPath) {
    std::string args = "--port=" + std::to_string(port);
    if (!worldPath.empty()) args += " --world=\"" + worldPath + "\"";   // quote: tolerate spaces in path
    return args;
}

ServerStatus MapExitCodeToStatus(DWORD exitCode) {
    if (exitCode == STILL_ACTIVE) return ServerStatus::Running;
    if (exitCode == 0)            return ServerStatus::Stopped;
    return ServerStatus::Crashed;
}

ServerSupervisor::ServerSupervisor() = default;
ServerSupervisor::~ServerSupervisor() {
    if (m_Process) Stop(1000);
    CloseHandles();
}

bool ServerSupervisor::Start(uint16_t port, const std::string& worldPath, const std::string& exePath) {
    if (Status() == ServerStatus::Running) {
        SM_WARN("ServerSupervisor: server already running on port %u", (unsigned)m_Port);
        return false;
    }
    CloseHandles();
    m_Port = port;
    m_LastExitCode = 0;

    m_Job = CreateJobObjectA(nullptr, nullptr);
    if (!m_Job) { SM_ERROR("ServerSupervisor: CreateJobObject failed (%lu)", GetLastError()); return false; }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(m_Job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)))
        SM_WARN("ServerSupervisor: SetInformationJobObject failed (%lu); orphan protection off", GetLastError());

    std::string cmd = "\"" + exePath + "\" " + BuildServerArgs(port, worldPath);
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                                   CREATE_SUSPENDED | CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    if (!ok) {
        SM_ERROR("ServerSupervisor: CreateProcess('%s') failed (%lu)", cmd.c_str(), GetLastError());
        CloseHandles();
        return false;
    }
    if (!AssignProcessToJobObject(m_Job, pi.hProcess))
        SM_WARN("ServerSupervisor: AssignProcessToJobObject failed (%lu)", GetLastError());
    ResumeThread(pi.hThread);

    m_Process = pi.hProcess;
    m_Thread  = pi.hThread;
    m_Status  = ServerStatus::Running;
    SM_TRACE("ServerSupervisor: spawned server.exe (pid %lu) on port %u", pi.dwProcessId, (unsigned)port);
    return true;
}

void ServerSupervisor::Stop(DWORD cleanWaitMs) {
    if (!m_Process) return;

    const std::string evName = ServerShutdownEventName(m_Port);
    HANDLE ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, evName.c_str());
    if (ev) { SetEvent(ev); CloseHandle(ev); }
    else    { SM_WARN("ServerSupervisor: OpenEvent('%s') failed (%lu); will terminate", evName.c_str(), GetLastError()); }

    if (WaitForSingleObject(m_Process, cleanWaitMs) != WAIT_OBJECT_0) {
        SM_WARN("ServerSupervisor: clean stop timed out; TerminateProcess");
        TerminateProcess(m_Process, 1);
        WaitForSingleObject(m_Process, 1000);
    }
    const ServerStatus captured = Status();   // captures Crashed + exit code if the server exited non-zero
    CloseHandles();
    // Don't mask a crash-on-shutdown as a clean stop; only downgrade to Stopped if it wasn't a crash.
    if (captured != ServerStatus::Crashed) m_Status = ServerStatus::Stopped;
}

ServerStatus ServerSupervisor::Status() {
    if (!m_Process) return m_Status;
    DWORD code = 0;
    if (GetExitCodeProcess(m_Process, &code)) {
        m_Status = MapExitCodeToStatus(code);
        if (m_Status != ServerStatus::Running) m_LastExitCode = code;
    }
    return m_Status;
}

void ServerSupervisor::CloseHandles() {
    if (m_Thread)  { CloseHandle(m_Thread);  m_Thread  = nullptr; }
    if (m_Process) { CloseHandle(m_Process); m_Process = nullptr; }
    if (m_Job)     { CloseHandle(m_Job);     m_Job     = nullptr; }
}
