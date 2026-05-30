#pragma once

#include <memory>

#include "netlib/netlib_api.h"
#include "netlib/IIo.h"

namespace netlib {

// Returns a static version string. Smoke-test hook to prove the DLL links + loads.
NETLIB_API const char* Version();

// Concrete adapter factories. The returned object owns its own threading.
NETLIB_API std::unique_ptr<IIoServer> MakeTcpServer();
NETLIB_API std::unique_ptr<IIoClient> MakeTcpClient();

// In-memory loopback pair: events delivered synchronously on the caller's thread.
// The client's Send reaches the server sink and vice versa. No socket, no OS
// permission, deterministic — for tests + in-process use.
struct InMemoryPair {
    std::unique_ptr<IIoServer> server;
    std::unique_ptr<IIoClient> client;
};
NETLIB_API InMemoryPair MakeInMemoryPair();

} // namespace netlib
