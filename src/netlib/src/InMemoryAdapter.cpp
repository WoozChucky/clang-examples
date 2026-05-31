#include "netlib/netlib.h"
#include "netlib/OwnedBuffer.h"

#include <memory>

namespace netlib {
namespace {

// Shared channel between the linked in-memory client + server. Synchronous:
// a Send immediately invokes the peer's sink on the caller's thread.
//
// THREAD-SAFETY: this is a SYNCHRONOUS, SINGLE-THREADED loopback. The channel
// state below (serverUp/clientUp/sink pointers) is unsynchronized, so Send/Start/Stop
// on the pair must be called from one thread at a time. Intended for deterministic
// tests and single-threaded in-process use; the sink contract still requires the *sink*
// itself be thread-safe. Concurrent use from multiple threads is unsupported (unlike TCP).
struct InMemChannel {
    IIoSink* serverSink = nullptr;   // server's sink (receives client->server)
    IIoSink* clientSink = nullptr;   // client's sink (receives server->client)
    bool     serverUp   = false;
    bool     clientUp   = false;
    static constexpr ConnId kClientConn = ConnId{1};
};

void Emit(IIoSink* sink, IoEvent::Kind kind, ConnId conn,
          std::span<const std::byte> payload = {}) {
    if (!sink) return;
    IoEvent ev{};
    ev.kind = kind;
    ev.conn = conn;
    ev.payload = payload;
    sink->OnIoEvent(ev);
}

class InMemServer final : public IIoServer {
public:
    explicit InMemServer(std::shared_ptr<InMemChannel> ch) : m_Ch(std::move(ch)) {}
    ~InMemServer() override { Stop(); }
    bool Start(const Endpoint&, const ConnConfig&, IIoSink* sink) override {
        m_Ch->serverSink = sink;
        m_Ch->serverUp = true;
        // If the client is already up, surface its connection now.
        if (m_Ch->clientUp) Emit(m_Ch->serverSink, IoEvent::Kind::Connected, InMemChannel::kClientConn);
        return true;
    }
    void Send(ConnId, OwnedBuffer&& payload) override {
        if (m_Ch->clientUp)
            Emit(m_Ch->clientSink, IoEvent::Kind::Message, ConnId::Invalid,
                 std::span<const std::byte>(payload.Payload(), payload.PayloadLen()));
        // payload destructs here (deleter reclaims) AFTER the synchronous sink call returns
    }
    void Close(ConnId) override { Stop(); }
    uint16_t BoundPort() const override { return 0; }
    void Stop() override {
        if (!m_Ch->serverUp) return;
        m_Ch->serverUp = false;
        if (m_Ch->clientUp) Emit(m_Ch->clientSink, IoEvent::Kind::Disconnected, ConnId::Invalid);
    }
private:
    std::shared_ptr<InMemChannel> m_Ch;
};

class InMemClient final : public IIoClient {
public:
    explicit InMemClient(std::shared_ptr<InMemChannel> ch) : m_Ch(std::move(ch)) {}
    ~InMemClient() override { Stop(); }
    bool Start(const Endpoint&, const ConnConfig&, IIoSink* sink) override {
        m_Ch->clientSink = sink;
        m_Ch->clientUp = true;
        Emit(m_Ch->clientSink, IoEvent::Kind::Connected, ConnId::Invalid);
        if (m_Ch->serverUp) Emit(m_Ch->serverSink, IoEvent::Kind::Connected, InMemChannel::kClientConn);
        return true;
    }
    void Send(OwnedBuffer&& payload) override {
        if (m_Ch->serverUp)
            Emit(m_Ch->serverSink, IoEvent::Kind::Message, InMemChannel::kClientConn,
                 std::span<const std::byte>(payload.Payload(), payload.PayloadLen()));
    }
    void Stop() override {
        if (!m_Ch->clientUp) return;
        m_Ch->clientUp = false;
        if (m_Ch->serverUp)
            Emit(m_Ch->serverSink, IoEvent::Kind::Disconnected, InMemChannel::kClientConn);
    }
private:
    std::shared_ptr<InMemChannel> m_Ch;
};

} // namespace

InMemoryPair MakeInMemoryPair() {
    auto ch = std::make_shared<InMemChannel>();
    InMemoryPair p;
    p.server = std::make_unique<InMemServer>(ch);
    p.client = std::make_unique<InMemClient>(ch);
    return p;
}

} // namespace netlib
