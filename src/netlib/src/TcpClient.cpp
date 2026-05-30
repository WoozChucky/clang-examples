#include "netlib/netlib.h"
#include "IocpCore.h"
#include "WinsockGuard.h"

#include <atomic>
#include <thread>
#include <ws2tcpip.h>

#include "lib.h"

namespace netlib {
namespace {

class TcpClient final : public IIoClient {
public:
    bool Start(const Endpoint& target, const ConnConfig& cfg, IIoSink* sink) override {
        if (!m_Wsa.Ok()) return false;
        if (!m_Core.Start(sink, cfg, /*workerCount*/ 1)) return false;
        m_Sink = sink;   // store for connect-failure notification (see ConnectLoop)
        m_Connecting.store(true, std::memory_order_release);
        // Blocking connect on a dedicated thread so Start() returns immediately and
        // the caller is never blocked; the socket then lives in IocpCore (overlapped).
        m_ConnectThread = std::thread([this, target]{ ConnectLoop(target); });
        return true;
    }

    void Send(std::span<const std::byte> payload) override {
        const ConnId id = m_Conn.load(std::memory_order_acquire);
        if (id != ConnId::Invalid) m_Core.Send(id, payload);
    }

    void Stop() override {
        m_Connecting.store(false, std::memory_order_release);
        // Ordering invariant: join the connect thread BEFORE m_Core.Stop() so
        // Register() can never run on a stopped core. Do not reorder.
        if (m_ConnectThread.joinable()) m_ConnectThread.join();
        m_Core.Stop();
    }
    ~TcpClient() override { Stop(); }

private:
    // Emit an Error event (invalid conn, empty payload) on a genuine connect
    // failure so a caller watching the sink can drive connect-retry instead of
    // waiting forever. Runs on the connect thread; the sink contract requires
    // thread-safety, so that's fine.
    void NotifyConnectFailure() {
        if (m_Sink) {
            netlib::IoEvent ev{};
            ev.kind = netlib::IoEvent::Kind::Error;
            ev.conn = netlib::ConnId::Invalid;
            m_Sink->OnIoEvent(ev);
        }
    }

    void ConnectLoop(const Endpoint& target) {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            SM_ERROR("netlib: client socket failed (%d)", WSAGetLastError());
            NotifyConnectFailure(); return;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(target.port);
        if (inet_pton(AF_INET, target.host.c_str(), &addr.sin_addr) != 1) {
            SM_ERROR("netlib: client bad host '%s'", target.host.c_str());
            ::closesocket(s); NotifyConnectFailure(); return;
        }
        // Stop()-requested cancellation before we block in connect(): intentional,
        // NOT a failure — bail silently (no Error event).
        if (!m_Connecting.load(std::memory_order_acquire)) { ::closesocket(s); return; }
        // NOTE: a connect() already in progress is NOT interruptible — Stop()
        // blocks until it returns (up to the OS TCP connect timeout for a dead
        // host). Acceptable for Phase 1 (loopback/LAN); a future non-blocking
        // connect + cancel-event would make Stop() responsive here.
        if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            SM_WARN("netlib: client connect %s:%u failed (%d)", target.host.c_str(), target.port, WSAGetLastError());
            ::closesocket(s); NotifyConnectFailure(); return;
        }
        const ConnId id = m_Core.Register(s);    // emits Connected + posts recv
        m_Conn.store(id, std::memory_order_release);
    }

    IIoSink*              m_Sink = nullptr;
    detail::WinsockGuard  m_Wsa;
    detail::IocpCore      m_Core;
    std::atomic<ConnId>   m_Conn{ ConnId::Invalid };
    std::atomic<bool>     m_Connecting{false};
    std::thread           m_ConnectThread;
};

} // namespace

std::unique_ptr<IIoClient> MakeTcpClient() { return std::make_unique<TcpClient>(); }

} // namespace netlib
