#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <span>

#include <winsock2.h>

#include "netlib/Endpoint.h"
#include "netlib/IoEvent.h"
#include "netlib/IIo.h"
#include "netlib/OwnedBuffer.h"
#include "Framer.h"

namespace netlib::detail {

struct Conn;

// A single overlapped operation. `ov` MUST be the first member (CONTAINING_RECORD).
// Holds a strong ref to its Conn so the Conn (and these buffers) outlive the
// completion. While any IoOp exists, its Conn is guaranteed alive.
struct IoOp {
    OVERLAPPED ov{};
    enum class Type : uint8_t { Recv, Send } type;
    std::vector<std::byte> recvBuffer;   // recv scratch (removed in Task 3)
    OwnedBuffer            sendBuffer;    // owns the framed bytes for a send op
    WSABUF wsabuf{};
    std::shared_ptr<Conn> conn;      // strong ref: keeps Conn alive while op in flight
};

// Per-connection state. Lifetime is managed by std::shared_ptr: the map holds one
// ref, each in-flight IoOp holds one ref. The Conn is freed exactly once when the
// last ref drops — no manual refcount/coordination needed.
struct Conn {
    std::atomic<SOCKET> sock{INVALID_SOCKET};
    ConnId  id   = ConnId::Invalid;
    Framer  framer;
    std::mutex            sendMx;
    std::deque<OwnedBuffer> sendQ;
    bool                  sending = false;
    std::atomic<bool>     closing{false};   // stops recv reposts; gates Disconnected-once

    explicit Conn(uint32_t maxFrame) : framer(maxFrame) {}
};

// Shared IOCP engine used by both TcpServer and TcpClient. Owns the completion
// port + worker pool. Connections are registered via Register(); the workers run
// recv->frame->sink and drain send queues. Stop() cancels + joins everything.
class IocpCore {
public:
    IocpCore() = default;
    ~IocpCore();

    bool Start(IIoSink* sink, const ConnConfig& cfg, int workerCount);
    // Take ownership of an accepted/connected socket; post the first recv. Returns its ConnId.
    ConnId Register(SOCKET sock);
    void Send(ConnId id, OwnedBuffer&& payload);
    void Close(ConnId id);
    void Stop();   // idempotent; cancels ops, posts exit packets, joins workers

private:
    void WorkerLoop();
    void PostRecv(const std::shared_ptr<Conn>& c);
    void PostSend(const std::shared_ptr<Conn>& c);  // call with sendMx held; posts head of queue
    void HandleRecv(const std::shared_ptr<Conn>& c, IoOp* op, DWORD bytes, bool ok);
    void HandleSend(const std::shared_ptr<Conn>& c, IoOp* op, DWORD bytes, bool ok);
    void Emit(ConnId id, IoEvent::Kind kind, std::span<const std::byte> payload = {});
    void Disconnect(const std::shared_ptr<Conn>& c); // emit Disconnected once + close socket + drop map ref

    HANDLE       m_Iocp = nullptr;
    IIoSink*     m_Sink = nullptr;
    ConnConfig   m_Cfg{};
    std::vector<std::thread> m_Workers;
    std::atomic<bool> m_Running{false};

    std::mutex   m_Mx;                       // guards m_Conns + m_NextId
    std::unordered_map<uint64_t, std::shared_ptr<Conn>> m_Conns;
    uint64_t     m_NextId = 1;               // ConnId 0 reserved invalid
};

} // namespace netlib::detail
