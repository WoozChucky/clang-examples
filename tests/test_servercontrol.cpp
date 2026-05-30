// Unit tests for the pure Phase-3 control helpers (no Win32, no sockets).
#include <cassert>
#include <cstdio>
#include <string>

#include "AppRole.h"
#include "ServerControl.h"

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
    std::printf("All server-control tests passed.\n");
    return 0;
}
