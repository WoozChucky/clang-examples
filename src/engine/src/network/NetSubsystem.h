#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Engine.h"
#include "MpscRing.h"
#include "NetBufferPool.h"
#include "NetServices.h"
#include <netlib/netlib.h>

// Engine-side networking singleton (GameThread-only API; netlib adapter threads call
// the per-adapter sinks). Owns the registry, the inbound MPSC ring, the payload pool.
// NOTE (Phase 4): singleton matches NavMeshSystem; in-process dual-world will require
// de-singletoning (umbrella spec section 9).
class ENGINE_API NetSubsystem {
public:
    static NetSubsystem& Instance();

    void Init();        // (re)initialize pool + ring; safe to call repeatedly (Shutdown first)
    void Shutdown();    // close all adapters (joins their threads), clear state

    NetHandle CreateServer(NetServerFactory factory, const NetServerConfig& cfg);
    NetHandle CreateClient(NetClientFactory factory, const NetClientConfig& cfg);
    uint16_t  BoundPort(NetHandle h);
    SendBuffer AcquireSend(size_t payloadBytes);
    bool       Send(NetHandle h, NetConnId conn, SendBuffer buf, uint32_t payloadLen);
    void       AbortSend(SendBuffer buf);
    bool      PollEvent(NetEvent* out);
    void      Close(NetHandle h);

    void      ReleaseGameResidentConnections();   // Model B: tear down Game.dll-resident adapters

private:
    NetSubsystem() = default;

    struct AdapterSink final : netlib::IIoSink {
        NetSubsystem* self = nullptr;
        NetHandle     handle = NetHandle::Invalid;
        void OnIoEvent(const netlib::IoEvent& ev) override { self->OnAdapterEvent(handle, ev); }
    };

    struct Entry {
        std::unique_ptr<netlib::IIoServer> server;
        std::unique_ptr<netlib::IIoClient> client;
        std::unique_ptr<AdapterSink>       sink;
        bool gameResident = false;
    };

    struct RingEvent {
        NetEventKind kind;
        NetHandle    adapter;
        NetConnId    conn;
        uint32_t     poolIndex;
        uint32_t     len;
        bool         hasPayload;
    };

    void OnAdapterEvent(NetHandle h, const netlib::IoEvent& ev);

    static constexpr size_t kRingSize  = 4096;
    static constexpr size_t kBlockSize = 64 * 1024;
    static constexpr size_t kBlocks    = 1024;
    static constexpr size_t kSendBlockSize = 16 * 1024;
    static constexpr size_t kSendBlocks    = 1024;

    std::mutex m_Mx;
    std::unordered_map<uint32_t, Entry> m_Adapters;
    uint32_t m_NextHandle = 1;

    std::unique_ptr<MpscRing<RingEvent, kRingSize>> m_Ring;
    std::unique_ptr<NetBufferPool>                  m_Pool;
    std::unique_ptr<NetBufferPool>                  m_SendPool;     // outbound send buffers (16 KB blocks)
    uint32_t m_BorrowedIndex = UINT32_MAX;          // inbound block lent to the game until next PollEvent
};
