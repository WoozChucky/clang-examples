#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "netlib/Endpoint.h"
#include "netlib/IoEvent.h"
#include "netlib/OwnedBuffer.h"

namespace netlib {

// Implemented by the CALLER. Called from adapter-owned thread(s) — for the IOCP
// server, concurrently from many worker threads. MUST be thread-safe. Copy
// IoEvent::payload before returning (it is borrowed).
class IIoSink {
public:
    virtual ~IIoSink() = default;
    virtual void OnIoEvent(const IoEvent& ev) = 0;
};

// Outbound client connection. Owns its async I/O thread(s); Stop() joins them.
class IIoClient {
public:
    virtual ~IIoClient() = default;
    // Connect to `target` and begin async I/O, pushing events to `sink`.
    // Returns false if the connect could not be initiated. `sink` must outlive Stop().
    virtual bool Start(const Endpoint& target, const ConnConfig& cfg, IIoSink* sink) = 0;
    // Enqueue one framed message (thread-safe). Takes ownership of a framed-payload
    // buffer (length head reserved); writes the length prefix in place.
    virtual void Send(OwnedBuffer&& payload) = 0;
    // Stop async I/O and join all threads. Idempotent. No OnIoEvent fires after it returns.
    virtual void Stop() = 0;
};

// Listening server. Owns its accept thread + IOCP worker pool; Stop() joins them.
class IIoServer {
public:
    virtual ~IIoServer() = default;
    virtual bool Start(const Endpoint& bind, const ConnConfig& cfg, IIoSink* sink) = 0;
    // Send to one connection (thread-safe). Takes ownership of a framed-payload
    // buffer (length head reserved); writes the length prefix in place.
    virtual void Send(ConnId conn, OwnedBuffer&& payload) = 0;
    virtual void Close(ConnId conn) = 0;     // drop one connection
    virtual void Stop() = 0;                 // stop accepting + join all workers
    // Port actually bound (resolves an ephemeral port chosen for port 0). 0 if not started.
    virtual uint16_t BoundPort() const = 0;
};

} // namespace netlib
