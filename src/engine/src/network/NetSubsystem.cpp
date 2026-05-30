#include "network/NetSubsystem.h"

#include <cstring>

#include "lib.h"

namespace { constexpr uint32_t kOpcodeBytes = 2; }

NetSubsystem& NetSubsystem::Instance() { static NetSubsystem s; return s; }

void NetSubsystem::Init() {
    Shutdown();
    m_Ring = std::make_unique<MpscRing<RingEvent, kRingSize>>();
    m_Pool = std::make_unique<NetBufferPool>(kBlockSize, kBlocks);
    m_DrainBuf.resize(kBlockSize);
}

void NetSubsystem::Shutdown() {
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

bool NetSubsystem::Send(NetHandle h, NetConnId conn, uint16_t opcode, const uint8_t* data, size_t len) {
    std::vector<std::byte> buf;
    buf.resize(kOpcodeBytes + len);
    buf[0] = static_cast<std::byte>(opcode & 0xff);
    buf[1] = static_cast<std::byte>((opcode >> 8) & 0xff);
    if (len) std::memcpy(buf.data() + kOpcodeBytes, data, len);

    std::scoped_lock lk(m_Mx);
    auto it = m_Adapters.find(static_cast<uint32_t>(h));
    if (it == m_Adapters.end()) return false;
    if (it->second.server) { it->second.server->Send(netlib::ConnId{ conn }, buf); return true; }
    if (it->second.client) { it->second.client->Send(buf); return true; }
    return false;
}

void NetSubsystem::OnAdapterEvent(NetHandle h, const netlib::IoEvent& ev) {
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
            if (p.size() < kOpcodeBytes) { SM_WARN("NetSubsystem: runt frame (%zu bytes); dropped", p.size()); return; }
            re.opcode = static_cast<uint16_t>(static_cast<uint8_t>(p[0])) |
                        (static_cast<uint16_t>(static_cast<uint8_t>(p[1])) << 8);
            const size_t payloadLen = p.size() - kOpcodeBytes;
            if (payloadLen > m_Pool->BlockSize()) { SM_WARN("NetSubsystem: payload %zu > block %zu; dropped", payloadLen, m_Pool->BlockSize()); return; }
            uint32_t idx;
            std::byte* block = m_Pool->Acquire(idx);
            if (!block) { SM_WARN("NetSubsystem: pool exhausted; message dropped"); return; }
            if (payloadLen) std::memcpy(block, p.data() + kOpcodeBytes, payloadLen);
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
    RingEvent re{};
    if (!m_Ring->Dequeue(re)) return false;
    out->kind    = re.kind;
    out->adapter = re.adapter;
    out->conn    = re.conn;
    out->opcode  = re.opcode;
    out->payload = nullptr;
    out->len     = 0;
    if (re.hasPayload) {
        if (m_DrainBuf.size() < re.len) m_DrainBuf.resize(re.len);
        std::memcpy(m_DrainBuf.data(), m_Pool->Block(re.poolIndex), re.len);
        m_Pool->Release(re.poolIndex);
        out->payload = m_DrainBuf.data();
        out->len     = re.len;
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
