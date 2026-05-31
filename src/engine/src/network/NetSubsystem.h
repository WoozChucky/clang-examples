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
#include <memory/IAllocator.h>

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

    // Read-only IAllocator view over a NetBufferPool, so the editor Memory panel can
    // observe block usage alongside the engine allocators. Allocate/Deallocate are
    // no-ops (the panel only reads Stats/Name/Category); Stats() is recomputed from the
    // pool's fixed capacity and its atomic in-use count on each read.
    struct PoolStatsAdapter final : Engine::IAllocator {
        const NetBufferPool* pool = nullptr;
        const char*          name = "";
        mutable Engine::AllocatorStats stats;
        void* Allocate(size_t, size_t) override { return nullptr; }
        void  Deallocate(void*, size_t) override {}
        const Engine::AllocatorStats& Stats() const override {
            const size_t blk = pool ? pool->BlockSize() : 0;
            stats.Capacity = pool ? pool->BlockCount() * blk : 0;
            stats.Used     = pool ? pool->InUse() * blk : 0;
            // Peak comes from the pool's TRUE high-water (recorded at Acquire), not a
            // read-time sample of Used — short-lived send buffers would otherwise never
            // be caught between panel frames and Peak would read 0.
            stats.Peak     = pool ? pool->PeakInUse() * blk : 0;
            return stats;
        }
        Engine::MemCategory Category() const override { return Engine::MemCategory::General; }
        const char* Name() const override { return name; }
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
    bool     m_SendHeapFallbackWarned = false;      // warn-once latch for send-pool-exhausted heap fallback (GameThread-only)

    PoolStatsAdapter m_RecvStats;   // registered with Engine::Registry() over m_Pool
    PoolStatsAdapter m_SendStats;   // registered with Engine::Registry() over m_SendPool
};
