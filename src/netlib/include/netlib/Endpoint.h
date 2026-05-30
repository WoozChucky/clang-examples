#pragma once

#include <cstdint>
#include <string>

namespace netlib {

// A connection id, unique within one IIoServer (servers fan out to many peers).
// 0 is reserved as "invalid / not a specific connection" (used for client-side
// events where there is only one connection).
enum class ConnId : uint64_t { Invalid = 0 };

struct Endpoint {
    std::string host;   // e.g. "127.0.0.1" or a hostname
    uint16_t    port = 0;
};

// Per-connection tunables. Defaults chosen for real-time game traffic.
struct ConnConfig {
    bool     noDelay      = true;       // TCP_NODELAY on — disable Nagle (~40ms batching is fatal)
    int      sendBufBytes = 1 << 18;    // SO_SNDBUF (256 KiB); 0 = leave OS default
    int      recvBufBytes = 1 << 18;    // SO_RCVBUF
    uint32_t maxFrameBytes = 1u << 24;  // 16 MiB hard cap; oversize length prefix => disconnect
};

} // namespace netlib
