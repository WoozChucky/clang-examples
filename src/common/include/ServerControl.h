#pragma once
#include <cstdint>
#include <string>

// Shared client/server/editor networking-control constants + pure helpers.
// Header-only and Win32-free so tests and all three exes can include it.
inline constexpr uint16_t kDedicatedServerDefaultPort = 27015;

inline std::string ServerShutdownEventName(uint16_t port) {
    return "smengine_server_shutdown_" + std::to_string(port);
}
