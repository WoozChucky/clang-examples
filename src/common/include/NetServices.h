#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <netlib/netlib.h>   // Endpoint, ConnConfig, ConnId, IIoServer/IIoClient, factories

// Engine-provided networking service table, threaded through SystemContext like
// NavServices. Engine populates it (NetServicesImpl::Init); GameThread threads a
// pointer each tick. All calls are GameThread-only (matches NavServices). Field
// order is append-only for Game.dll binary compat.

enum class NetHandle : uint32_t { Invalid = 0 };   // identifies a registered adapter

// Game-facing connection id (mirrors netlib::ConnId; servers fan out to many peers).
using NetConnId = uint64_t;
inline constexpr NetConnId kNetConnInvalid = 0;

enum class NetEventKind : uint8_t { Connected, Disconnected, Error, Message };

// One drained inbound event. `payload` (Message only) points into NetSubsystem's
// inbound pool block — valid until the next PollEvent. Copy it to retain.
struct NetEvent {
    NetEventKind   kind   = NetEventKind::Error;
    NetHandle      adapter = NetHandle::Invalid;   // which registered adapter
    NetConnId      conn   = kNetConnInvalid;       // which connection within it
    const uint8_t* payload = nullptr;              // opaque payload frame (no opcode — type tagging is the caller's concern); borrowed until next PollEvent
    uint32_t       len    = 0;
};

// A writable send buffer handed to the caller by AcquireSend. The caller serializes
// directly into `data` (up to `cap` bytes), then passes it to Send. `token` is an
// opaque engine handle (do not interpret). Engine reserves a length head before `data`.
struct SendBuffer {
    uint8_t* data  = nullptr;   // writable payload region
    uint32_t cap   = 0;         // bytes available at `data`
    uint64_t token = 0;         // opaque; identifies the backing block/heap for Send/AbortSend
};

struct NetServerConfig {
    netlib::Endpoint   bind{};
    netlib::ConnConfig conn{};
    bool               gameResident = false;   // true => Game.dll-resident adapter (Model B teardown)
};
struct NetClientConfig {
    netlib::Endpoint   target{};
    netlib::ConnConfig conn{};
    bool               gameResident = false;
};

// Adapter factories the game supplies (e.g. &netlib::MakeTcpServer).
using NetServerFactory = std::unique_ptr<netlib::IIoServer>(*)();
using NetClientFactory = std::unique_ptr<netlib::IIoClient>(*)();

struct NetServices {
    // Create a listening server / outbound client from a netlib factory. Engine owns
    // the adapter, wires its sink, and Starts it. Returns Invalid on failure.
    NetHandle (*CreateServer)(NetServerFactory factory, const NetServerConfig& cfg);
    NetHandle (*CreateClient)(NetClientFactory factory, const NetClientConfig& cfg);

    // Server's actually-bound port (resolves ephemeral port 0); 0 if not a server / not bound.
    uint16_t  (*BoundPort)(NetHandle h);

    // Acquire a writable send buffer sized for `payloadBytes`. Caller serializes into
    // SendBuffer::data, then calls Send (or AbortSend to discard). GameThread-only.
    SendBuffer (*AcquireSend)(size_t payloadBytes);
    // Send a previously-acquired buffer to a connection (consumes it). `payloadLen`
    // is the number of bytes actually written (<= the acquired cap). Client adapters
    // pass kNetConnInvalid. Returns false on unknown handle (buffer is still released).
    bool (*Send)(NetHandle h, NetConnId conn, SendBuffer buf, uint32_t payloadLen);
    // Discard an acquired buffer without sending (releases the backing block/heap).
    void (*AbortSend)(SendBuffer buf);

    // Drain ONE event from the shared inbound MPSC ring (all adapters). False when empty.
    bool      (*PollEvent)(NetEvent* out);

    // Close + tear down one adapter (joins its threads via netlib Stop()).
    void      (*Close)(NetHandle h);
};
