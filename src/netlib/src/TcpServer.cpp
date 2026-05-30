#include "netlib/netlib.h"
#include "IocpCore.h"
#include "WinsockGuard.h"

#include <atomic>
#include <thread>
#include <ws2tcpip.h>

#include "lib.h"

namespace netlib {
namespace {

class TcpServer final : public IIoServer {
public:
    bool Start(const Endpoint& bind, const ConnConfig& cfg, IIoSink* sink) override {
        if (!m_Wsa.Ok()) return false;
        m_Listen = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_Listen == INVALID_SOCKET) { SM_ERROR("netlib: listen socket failed (%d)", WSAGetLastError()); return false; }
        BOOL reuse = TRUE;
        setsockopt(m_Listen, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(bind.port);
        inet_pton(AF_INET, bind.host.empty() ? "0.0.0.0" : bind.host.c_str(), &addr.sin_addr);
        if (::bind(m_Listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            SM_ERROR("netlib: bind %s:%u failed (%d)", bind.host.c_str(), bind.port, WSAGetLastError());
            ::closesocket(m_Listen); m_Listen = INVALID_SOCKET; return false;
        }
        // Read back the actually-bound port (resolves port 0).
        sockaddr_in got{}; int gotLen = sizeof(got);
        if (getsockname(m_Listen, reinterpret_cast<sockaddr*>(&got), &gotLen) == 0)
            m_Port = ntohs(got.sin_port);

        if (::listen(m_Listen, SOMAXCONN) != 0) {
            SM_ERROR("netlib: listen failed (%d)", WSAGetLastError());
            ::closesocket(m_Listen); m_Listen = INVALID_SOCKET; return false;
        }
        // Worker pool sized to hardware concurrency (clamped); fine to start small.
        int workers = static_cast<int>(std::thread::hardware_concurrency());
        if (workers < 1) workers = 1; if (workers > 8) workers = 8;
        if (!m_Core.Start(sink, cfg, workers)) { ::closesocket(m_Listen); m_Listen = INVALID_SOCKET; return false; }

        m_Accepting.store(true, std::memory_order_release);
        m_AcceptThread = std::thread([this]{ AcceptLoop(); });
        SM_TRACE("netlib: TcpServer listening on %s:%u", bind.host.c_str(), m_Port);
        return true;
    }

    void Send(ConnId conn, std::span<const std::byte> payload) override { m_Core.Send(conn, payload); }
    void Close(ConnId conn) override { m_Core.Close(conn); }
    uint16_t BoundPort() const override { return m_Port; }

    void Stop() override {
        if (!m_Accepting.exchange(false)) { m_Core.Stop(); return; }
        if (m_Listen != INVALID_SOCKET) { ::closesocket(m_Listen); m_Listen = INVALID_SOCKET; } // unblocks accept()
        if (m_AcceptThread.joinable()) m_AcceptThread.join();
        m_Core.Stop();
    }
    ~TcpServer() override { Stop(); }

private:
    void AcceptLoop() {
        while (m_Accepting.load(std::memory_order_acquire)) {
            SOCKET s = ::accept(m_Listen, nullptr, nullptr);
            if (s == INVALID_SOCKET) {
                if (!m_Accepting.load(std::memory_order_acquire)) break;  // closed by Stop()
                const int err = WSAGetLastError();
                if (err == WSAEINTR || err == WSAENOTSOCK) break;
                SM_WARN("netlib: accept failed (%d)", err);
                continue;
            }
            m_Core.Register(s);
        }
    }

    detail::WinsockGuard m_Wsa;
    detail::IocpCore     m_Core;
    SOCKET               m_Listen = INVALID_SOCKET;
    uint16_t             m_Port   = 0;
    std::atomic<bool>    m_Accepting{false};
    std::thread          m_AcceptThread;
};

} // namespace

std::unique_ptr<IIoServer> MakeTcpServer() { return std::make_unique<TcpServer>(); }

} // namespace netlib
