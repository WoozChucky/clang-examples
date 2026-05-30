#include "IocpCore.h"

#include <cstring>

#include <mswsock.h>

#include "lib.h"   // SM_TRACE / SM_WARN / SM_ERROR

#pragma comment(lib, "ws2_32.lib")

namespace netlib::detail {

namespace {
constexpr ULONG_PTR kExitKey = 0;   // PostQueuedCompletionStatus exit sentinel

// Atomically take the socket and close it; only the caller that wins the
// exchange actually closes (prevents double-close of a recycled handle).
inline void CloseSock(std::atomic<SOCKET>& s) {
    SOCKET h = s.exchange(INVALID_SOCKET);
    if (h != INVALID_SOCKET) ::closesocket(h);
}
}   // namespace

IocpCore::~IocpCore() { Stop(); }

bool IocpCore::Start(IIoSink* sink, const ConnConfig& cfg, int workerCount) {
    m_Sink = sink;
    m_Cfg  = cfg;
    m_Iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (!m_Iocp) { SM_ERROR("netlib: CreateIoCompletionPort failed (%lu)", GetLastError()); return false; }
    m_Running.store(true, std::memory_order_release);
    if (workerCount < 1) workerCount = 1;
    for (int i = 0; i < workerCount; ++i) m_Workers.emplace_back([this]{ WorkerLoop(); });
    return true;
}

ConnId IocpCore::Register(SOCKET sock) {
    // Apply per-connection options (TCP_NODELAY + buffer sizes).
    if (m_Cfg.noDelay) {
        BOOL on = TRUE;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
    }
    if (m_Cfg.sendBufBytes > 0)
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&m_Cfg.sendBufBytes), sizeof(int));
    if (m_Cfg.recvBufBytes > 0)
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&m_Cfg.recvBufBytes), sizeof(int));

    std::shared_ptr<Conn> c;
    ConnId id{};
    {
        std::scoped_lock lk(m_Mx);
        const uint64_t n = m_NextId++;
        c = std::make_shared<Conn>(m_Cfg.maxFrameBytes);
        c->sock.store(sock, std::memory_order_release);
        c->id   = ConnId{n};
        m_Conns.emplace(n, c);
        id = ConnId{n};
    }
    // Associate the socket with the completion port; completion key = Conn* (used
    // only for association — actual access goes through op->conn so the Conn is
    // guaranteed alive for the duration of every completion).
    if (!CreateIoCompletionPort(reinterpret_cast<HANDLE>(sock), m_Iocp,
                                reinterpret_cast<ULONG_PTR>(c.get()), 0)) {
        SM_ERROR("netlib: associate socket to IOCP failed (%lu)", GetLastError());
        // No Connected was emitted yet (that happens below). Close the socket and
        // erase the half-registered Conn so it does not linger until Stop().
        CloseSock(c->sock);
        { std::scoped_lock lk(m_Mx); m_Conns.erase(static_cast<uint64_t>(id)); }
        return ConnId::Invalid;
    }
    Emit(id, IoEvent::Kind::Connected);
    PostRecv(c);
    return id;
}

void IocpCore::PostRecv(const std::shared_ptr<Conn>& c) {
    if (c->closing.load(std::memory_order_acquire)) return;
    auto* op = new IoOp();
    op->type = IoOp::Type::Recv;
    op->conn = c;                    // strong ref: keeps Conn alive while op in flight
    op->buffer.resize(64 * 1024);
    op->wsabuf.buf = reinterpret_cast<CHAR*>(op->buffer.data());
    op->wsabuf.len = static_cast<ULONG>(op->buffer.size());
    // Load the handle once. If it is already INVALID, a concurrent close won the
    // race — do not issue WSARecv on INVALID_SOCKET. Clean up the op and bail; the
    // close path will (or already did) drive Disconnect.
    const SOCKET h = c->sock.load(std::memory_order_acquire);
    if (h == INVALID_SOCKET) { delete op; return; }
    DWORD flags = 0, bytes = 0;
    const int rc = WSARecv(h, &op->wsabuf, 1, &bytes, &flags, &op->ov, nullptr);
    if (rc == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            if (err != WSAECONNRESET && err != WSAECONNABORTED)
                SM_WARN("netlib: WSARecv failed (%d)", err);
            delete op;            // drops this op's Conn ref
            Disconnect(c);        // safe: caller still holds a ref to `c`
        }
    }
}

void IocpCore::Send(ConnId id, std::span<const std::byte> payload) {
    // Look up + copy out the shared_ptr under m_Mx, then release m_Mx BEFORE
    // touching sendMx / issuing WSASend (avoids m_Mx re-entrancy + m_Mx->sendMx ABBA).
    std::shared_ptr<Conn> c;
    {
        std::scoped_lock lk(m_Mx);
        auto it = m_Conns.find(static_cast<uint64_t>(id));
        if (it == m_Conns.end()) return;
        c = it->second;                 // keeps `c` alive after we unlock m_Mx
    }

    // Frame: [uint32 LE length][payload], copied into a send buffer.
    std::vector<std::byte> buf;
    const uint32_t len = static_cast<uint32_t>(payload.size());
    buf.resize(4 + payload.size());
    std::memcpy(buf.data(), &len, 4);
    if (!payload.empty()) std::memcpy(buf.data() + 4, payload.data(), payload.size());

    std::scoped_lock slk(c->sendMx);    // m_Mx is NOT held here
    c->sendQ.push_back(std::move(buf));
    if (!c->sending) { c->sending = true; PostSend(c); }
}

void IocpCore::PostSend(const std::shared_ptr<Conn>& c) {   // sendMx held; m_Mx NOT held
    if (c->sendQ.empty()) { c->sending = false; return; }
    auto* op = new IoOp();
    op->type = IoOp::Type::Send;
    op->conn = c;                    // strong ref: keeps Conn alive while op in flight
    op->buffer = std::move(c->sendQ.front());
    c->sendQ.pop_front();
    op->wsabuf.buf = reinterpret_cast<CHAR*>(op->buffer.data());
    op->wsabuf.len = static_cast<ULONG>(op->buffer.size());
    // Load the handle once. If a concurrent close already invalidated it, do not
    // issue WSASend on INVALID_SOCKET: clear `sending` and drop the op. The close
    // path drives Disconnect; the queued buffer is discarded with the Conn.
    const SOCKET h = c->sock.load(std::memory_order_acquire);
    if (h == INVALID_SOCKET) { c->sending = false; delete op; return; }
    DWORD bytes = 0;
    const int rc = WSASend(h, &op->wsabuf, 1, &bytes, 0, &op->ov, nullptr);
    if (rc == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            if (err != WSAECONNRESET && err != WSAECONNABORTED)
                SM_WARN("netlib: WSASend failed (%d)", err);
            c->sending = false;
            delete op;            // drops this op's Conn ref
            // Disconnect only touches m_Mx for the map erase (not sendMx), and `c`
            // stays alive via the caller's ref — no re-entrancy of sendMx/m_Mx hazard.
            Disconnect(c);
        }
    }
}

void IocpCore::WorkerLoop() {
    for (;;) {
        DWORD bytes = 0; ULONG_PTR key = 0; OVERLAPPED* ov = nullptr;
        const BOOL ok = GetQueuedCompletionStatus(m_Iocp, &bytes, &key, &ov, INFINITE);
        if (key == kExitKey && ov == nullptr) return;   // exit packet
        if (!ov) continue;
        auto* op = CONTAINING_RECORD(ov, IoOp, ov);
        // Take the Conn from the op's strong ref (NOT the raw key) — guarantees the
        // Conn is alive for the whole handler. Move the ref out so the IoOp can be
        // deleted inside the handler while `c` stays alive locally.
        std::shared_ptr<Conn> c = std::move(op->conn);
        if (op->type == IoOp::Type::Recv) HandleRecv(c, op, bytes, ok == TRUE);
        else                              HandleSend(c, op, bytes, ok == TRUE);
    }
}

void IocpCore::HandleRecv(const std::shared_ptr<Conn>& c, IoOp* op, DWORD bytes, bool ok) {
    if (!ok || bytes == 0) {            // error or graceful peer close
        delete op;
        Disconnect(c);                  // `c` alive via local ref
        return;
    }
    const bool framerOk = c->framer.Push(
        std::span<const std::byte>(op->buffer.data(), bytes),
        [&](std::span<const std::byte> frame) { Emit(c->id, IoEvent::Kind::Message, frame); });
    delete op;
    if (!framerOk) { Disconnect(c); return; }      // oversize frame
    PostRecv(c);                                   // keep one recv outstanding (no-op if closing)
}

void IocpCore::HandleSend(const std::shared_ptr<Conn>& c, IoOp* op, DWORD /*bytes*/, bool ok) {
    delete op;
    if (!ok) { Disconnect(c); return; }            // `c` alive via local ref
    std::scoped_lock slk(c->sendMx);
    PostSend(c);                                   // send next queued frame, if any
}

void IocpCore::Emit(ConnId id, IoEvent::Kind kind, std::span<const std::byte> payload) {
    if (!m_Sink) return;
    IoEvent ev{}; ev.kind = kind; ev.conn = id; ev.payload = payload;
    m_Sink->OnIoEvent(ev);
}

void IocpCore::Disconnect(const std::shared_ptr<Conn>& c) {
    bool expected = false;
    if (!c->closing.compare_exchange_strong(expected, true)) return;  // once
    CloseSock(c->sock);   // exactly one path closes this handle (exchange winner)
    // Emit OUTSIDE any lock; `c` is alive (caller holds an op/local ref).
    Emit(c->id, IoEvent::Kind::Disconnected);
    // Drop the MAP's ref. In-flight ops keep `c` alive via their own refs; the Conn
    // is freed exactly once when the last ref (map ref + all op refs) drops.
    std::scoped_lock lk(m_Mx);
    m_Conns.erase(static_cast<uint64_t>(c->id));
}

void IocpCore::Close(ConnId id) {
    std::scoped_lock lk(m_Mx);
    auto it = m_Conns.find(static_cast<uint64_t>(id));
    if (it != m_Conns.end()) {
        // closesocket forces outstanding ops to complete with error → Disconnect path.
        CloseSock(it->second->sock);
    }
}

void IocpCore::Stop() {
    if (!m_Running.exchange(false)) return;
    // Close all sockets to force outstanding ops to complete.
    {
        std::scoped_lock lk(m_Mx);
        for (auto& [n, c] : m_Conns)
            CloseSock(c->sock);
    }
    // Wake every worker with an exit packet, then join.
    for (size_t i = 0; i < m_Workers.size(); ++i)
        PostQueuedCompletionStatus(m_Iocp, 0, kExitKey, nullptr);
    for (auto& t : m_Workers) if (t.joinable()) t.join();
    m_Workers.clear();
    // Drain any completions still queued after the workers exited (a real completion
    // can race the exit packet). Each leftover IoOp holds a Conn ref; deleting it
    // releases that ref so the Conn can free. Timed so we never block forever.
    for (;;) {
        DWORD bytes = 0; ULONG_PTR key = 0; OVERLAPPED* ov = nullptr;
        const BOOL ok = GetQueuedCompletionStatus(m_Iocp, &bytes, &key, &ov, 0 /*non-blocking*/);
        if (ov) {
            auto* op = CONTAINING_RECORD(ov, IoOp, ov);
            delete op;               // drops the op's shared_ptr<Conn> ref (even on failed I/O)
            continue;
        }
        if (!ok) break;              // queue empty (WAIT_TIMEOUT, ov==nullptr) — done
        // else: a stray exit packet (ov==nullptr, ok==TRUE) — skip and keep draining
    }
    { std::scoped_lock lk(m_Mx); m_Conns.clear(); }
    if (m_Iocp) { CloseHandle(m_Iocp); m_Iocp = nullptr; }
}

} // namespace netlib::detail
