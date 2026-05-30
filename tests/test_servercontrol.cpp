// Unit tests for the pure Phase-3 control helpers (no Win32, no sockets).
#include <cassert>
#include <cstdio>
#include <string>

#include "AppRole.h"
#include "ServerControl.h"
#include "ServerSupervisor.h"

int main() {
    assert(kDedicatedServerDefaultPort == 27015);
    const std::string a = ServerShutdownEventName(27015);
    const std::string b = ServerShutdownEventName(27016);
    assert(!a.empty());
    assert(a != b);
    assert(a.find("27015") != std::string::npos);
    AppRole r = AppRole::Client;
    assert(r == AppRole::Client);
    r = AppRole::Server;
    assert(r == AppRole::Server);

    // ServerSupervisor pure helpers (no process spawned).
    {
        const std::string args = BuildServerArgs(27015, "assets/world.json");
        assert(args.find("--port=27015") != std::string::npos);
        assert(args.find("--world=\"assets/world.json\"") != std::string::npos);

        const std::string args2 = BuildServerArgs(27016, "");
        assert(args2.find("--port=27016") != std::string::npos);
        assert(args2.find("--world=") == std::string::npos);

        assert(MapExitCodeToStatus(STILL_ACTIVE) == ServerStatus::Running);
        assert(MapExitCodeToStatus(0)            == ServerStatus::Stopped);
        assert(MapExitCodeToStatus(0xC0000005)   == ServerStatus::Crashed);
    }

    std::printf("All server-control tests passed.\n");
    return 0;
}
