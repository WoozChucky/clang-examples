#include "network/NetSubsystem.h"

#include <cstring>

#include <netlib/OwnedBuffer.h>

#include "lib.h"
#include <memory/AllocatorRegistry.h>

NetSubsystem& NetSubsystem::Instance() { static NetSubsystem s; return s; }

void NetSubsystem::Init() {
    Shutdown();
    m_Ring = std::make_unique<MpscRing<RingEvent, kRingSize>>();
    m_Pool = std::make_unique<NetBufferPool>(kBlockSize, kBlocks);
    m_SendPool = std::make_unique<NetBufferPool>(kSendBlockSize, kSendBlocks);
    m_BorrowedIndex = UINT32_MAX;

    // Surface both pools in the editor Memory panel (read-only observers).
    m_RecvStats.pool = m_Pool.get();     m_RecvStats.name = "Net Recv Pool";
    m_SendStats.pool = m_SendPool.get(); m_SendStats.name = "Net Send Pool";
    Engine::Registry().Register(&m_RecvStats);
    Engine::Registry().Register(&m_SendStats);
}

void NetSubsystem::Shutdown() {
    // Unregister observers before the pools they point at are reset (lifetime-safe).
    Engine::Registry().Unregister(&m_RecvStats);
    Engine::Registry().Unregister(&m_SendStats);
    m_RecvStats.pool = nullptr;
    m_SendStats.pool = nullptr;
    if (m_BorrowedIndex != UINT32_MAX && m_Pool) { m_Pool->Release(m_BorrowedIndex); m_BorrowedIndex = UINT32_MAX; }
    {
        std::scoped_lock lk(m_Mx);
        for (auto& [id, e] : m_Adapters) {
            if (e.server) e.server->Stop();
            if (e.client) e.client->Stop();
        }
        m_Adapters.clear();
    }
    m_Ring.reset();
    m_Pool.reset();
    m_SendPool.reset();
}

NetHandle NetSubsystem::CreateServer(NetServerFactory factory, const NetServerConfig& cfg) {
    if (!factory || !m_Ring) return NetHandle::Invalid;
    auto server = factory();
    if (!server) return NetHandle::Invalid;

    uint32_t id;
    AdapterSink* sinkPtr = nullptr;
    netlib::IIoServer* srv = server.get();   // valid after the unique_ptr is moved into the map
    {
        std::scoped_lock lk(m_Mx);
        id = m_NextHandle++;
        Entry e;
        e.gameResident = cfg.gameResident;
        e.sink = std::make_unique<AdapterSink>();
        e.sink->self = this;
        e.sink->handle = NetHandle{ id };
        sinkPtr = e.sink.get();
        e.server = std::move(server);
        m_Adapters.emplace(id, std::move(e));
    }
    if (!srv->Start(cfg.bind, cfg.conn, sinkPtr)) {
        SM_WARN("NetSubsystem: server Start failed");
        Close(NetHandle{ id });
        return NetHandle::Invalid;
    }
    return NetHandle{ id };
}

NetHandle NetSubsystem::CreateClient(NetClientFactory factory, const NetClientConfig& cfg) {
    if (!factory || !m_Ring) return NetHandle::Invalid;
    auto client = factory();
    if (!client) return NetHandle::Invalid;

    uint32_t id;
    AdapterSink* sinkPtr = nullptr;
    netlib::IIoClient* cli = client.get();   // valid after the unique_ptr is moved into the map
    {
        std::scoped_lock lk(m_Mx);
        id = m_NextHandle++;
        Entry e;
        e.gameResident = cfg.gameResident;
        e.sink = std::make_unique<AdapterSink>();
        e.sink->self = this;
        e.sink->handle = NetHandle{ id };
        sinkPtr = e.sink.get();
        e.client = std::move(client);
        m_Adapters.emplace(id, std::move(e));
    }
    if (!cli->Start(cfg.target, cfg.conn, sinkPtr)) {
        SM_WARN("NetSubsystem: client Start failed");
        Close(NetHandle{ id });
        return NetHandle::Invalid;
    }
    return NetHandle{ id };
}

uint16_t NetSubsystem::BoundPort(NetHandle h) {
    std::scoped_lock lk(m_Mx);
    auto it = m_Adapters.find(static_cast<uint32_t>(h));
    if (it == m_Adapters.end() || !it->second.server) return 0;
    return it->second.server->BoundPort();
}

namespace {
    constexpr uint64_t kPoolBit   = 1ull << 63;
    constexpr uint64_t kHeapToken = 0;           // token == 0 => heap-backed
    // OwnedBuffer deleters (run on the IOCP worker at send completion).
    void ReleaseToSendPool(void* poolCtx, std::byte* base) noexcept {
        auto* pool = static_cast<NetBufferPool*>(poolCtx);
        pool->Release(pool->IndexOf(base));      // base == block start (head at offset 0)
    }
    void FreeHeap(void*, std::byte* base) noexcept { delete[] base; }
}

SendBuffer NetSubsystem::AcquireSend(size_t payloadBytes) {
    SendBuffer sb{};
    const size_t total = netlib::OwnedBuffer::kHeadBytes + payloadBytes;
    if (m_SendPool && total <= m_SendPool->BlockSize()) {
        uint32_t idx;
        std::byte* block = m_SendPool->Acquire(idx);
        if (block) {
            sb.data  = reinterpret_cast<uint8_t*>(block + netlib::OwnedBuffer::kHeadBytes);
            sb.cap   = static_cast<uint32_t>(payloadBytes);
            sb.token = kPoolBit | idx;
            return sb;
        }
        // pool momentarily exhausted -> fall through to heap
    }
    auto* base = new std::byte[total];
    sb.data  = reinterpret_cast<uint8_t*>(base + netlib::OwnedBuffer::kHeadBytes);
    sb.cap   = static_cast<uint32_t>(payloadBytes);
    sb.token = kHeapToken;   // heap; base recovered as data-kHeadBytes
    return sb;
}

void NetSubsystem::AbortSend(SendBuffer buf) {
    if (!buf.data) return;
    std::byte* base = reinterpret_cast<std::byte*>(buf.data) - netlib::OwnedBuffer::kHeadBytes;
    if (buf.token & kPoolBit) { if (m_SendPool) m_SendPool->Release(static_cast<uint32_t>(buf.token & ~kPoolBit)); }
    else                        delete[] base;
}

bool NetSubsystem::Send(NetHandle h, NetConnId conn, SendBuffer buf, uint32_t payloadLen) {
    if (!buf.data) return false;
    std::byte* base = reinterpret_cast<std::byte*>(buf.data) - netlib::OwnedBuffer::kHeadBytes;

    netlib::OwnedBuffer owned =
        (buf.token & kPoolBit)
            ? netlib::OwnedBuffer(base, payloadLen, m_SendPool.get(), &ReleaseToSendPool)
            : netlib::OwnedBuffer(base, payloadLen, nullptr, &FreeHeap);

    std::scoped_lock lk(m_Mx);
    auto it = m_Adapters.find(static_cast<uint32_t>(h));
    if (it == m_Adapters.end()) return false;   // `owned` destructs -> deleter reclaims
    if (it->second.server) { it->second.server->Send(netlib::ConnId{ conn }, std::move(owned)); return true; }
    if (it->second.client) { it->second.client->Send(std::move(owned)); return true; }
    return false;
}

void NetSubsystem::OnAdapterEvent(NetHandle h, const netlib::IoEvent& ev) {
    // NOTE: this null-check is NOT the thread-safety mechanism. Safety comes from Shutdown()/Init()
    // joining all adapter threads (via netlib Stop()) BEFORE resetting m_Ring/m_Pool, so no adapter
    // thread runs concurrently with a reset; this guard only handles the not-yet-Init'd case.
    if (!m_Ring || !m_Pool) return;
    RingEvent re{};
    re.adapter = h;
    re.conn    = static_cast<NetConnId>(ev.conn);
    re.hasPayload = false;

    switch (ev.kind) {
        case netlib::IoEvent::Kind::Connected:    re.kind = NetEventKind::Connected; break;
        case netlib::IoEvent::Kind::Disconnected: re.kind = NetEventKind::Disconnected; break;
        case netlib::IoEvent::Kind::Error:        re.kind = NetEventKind::Error; break;
        case netlib::IoEvent::Kind::Message: {
            re.kind = NetEventKind::Message;
            const auto& p = ev.payload;
            const size_t payloadLen = p.size();
            if (payloadLen > m_Pool->BlockSize()) { SM_WARN("NetSubsystem: payload %zu > block %zu; dropped", payloadLen, m_Pool->BlockSize()); return; }
            uint32_t idx;
            std::byte* block = m_Pool->Acquire(idx);
            if (!block) { SM_WARN("NetSubsystem: pool exhausted; message dropped"); return; }
            if (payloadLen) std::memcpy(block, p.data(), payloadLen);
            re.poolIndex = idx;
            re.len = static_cast<uint32_t>(payloadLen);
            re.hasPayload = true;
            break;
        }
    }

    if (!m_Ring->Enqueue(re)) {
        SM_WARN("NetSubsystem: inbound ring full; event dropped");
        if (re.hasPayload) m_Pool->Release(re.poolIndex);
    }
}

bool NetSubsystem::PollEvent(NetEvent* out) {
    if (!out || !m_Ring) return false;
    // Release the block borrowed by the PREVIOUS PollEvent (the game has had its turn).
    if (m_BorrowedIndex != UINT32_MAX) { m_Pool->Release(m_BorrowedIndex); m_BorrowedIndex = UINT32_MAX; }

    RingEvent re{};
    if (!m_Ring->Dequeue(re)) return false;
    out->kind    = re.kind;
    out->adapter = re.adapter;
    out->conn    = re.conn;
    out->payload = nullptr;
    out->len     = 0;
    if (re.hasPayload) {
        out->payload = reinterpret_cast<const uint8_t*>(m_Pool->Block(re.poolIndex));   // borrowed until next PollEvent
        out->len     = re.len;
        m_BorrowedIndex = re.poolIndex;               // released at the top of the next PollEvent
    }
    return true;
}

void NetSubsystem::Close(NetHandle h) {
    std::unique_ptr<netlib::IIoServer> srv;
    std::unique_ptr<netlib::IIoClient> cli;
    std::unique_ptr<AdapterSink> sink;
    {
        std::scoped_lock lk(m_Mx);
        auto it = m_Adapters.find(static_cast<uint32_t>(h));
        if (it == m_Adapters.end()) return;
        srv  = std::move(it->second.server);
        cli  = std::move(it->second.client);
        sink = std::move(it->second.sink);
        m_Adapters.erase(it);
    }
    if (srv) srv->Stop();
    if (cli) cli->Stop();
    // srv/cli/sink destruct here, after their threads have joined.
}

void NetSubsystem::ReleaseGameResidentConnections() {
    std::vector<NetHandle> toClose;
    {
        std::scoped_lock lk(m_Mx);
        for (auto& [id, e] : m_Adapters) if (e.gameResident) toClose.push_back(NetHandle{ id });
    }
    for (NetHandle h : toClose) Close(h);
}
