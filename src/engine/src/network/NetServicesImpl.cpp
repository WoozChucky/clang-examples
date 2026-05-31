#include "network/NetServicesImpl.h"
#include "network/NetSubsystem.h"

#include <utility>   // std::move

namespace {
NetHandle FwdCreateServer(NetServerFactory f, const NetServerConfig& c) { return NetSubsystem::Instance().CreateServer(f, c); }
NetHandle FwdCreateClient(NetClientFactory f, const NetClientConfig& c) { return NetSubsystem::Instance().CreateClient(f, c); }
uint16_t  FwdBoundPort(NetHandle h) { return NetSubsystem::Instance().BoundPort(h); }
SendBuffer FwdAcquireSend(size_t n) { return NetSubsystem::Instance().AcquireSend(n); }
bool       FwdSend(NetHandle h, NetConnId conn, SendBuffer b, uint32_t n) { return NetSubsystem::Instance().Send(h, conn, std::move(b), n); }
void       FwdAbortSend(SendBuffer b) { NetSubsystem::Instance().AbortSend(std::move(b)); }
bool      FwdPollEvent(NetEvent* o) { return NetSubsystem::Instance().PollEvent(o); }
void      FwdClose(NetHandle h) { NetSubsystem::Instance().Close(h); }
}

namespace NetServicesImpl {
void Init(NetServices& out) {
    out.CreateServer = &FwdCreateServer;
    out.CreateClient = &FwdCreateClient;
    out.BoundPort    = &FwdBoundPort;
    out.AcquireSend  = &FwdAcquireSend;
    out.Send         = &FwdSend;
    out.AbortSend    = &FwdAbortSend;
    out.PollEvent    = &FwdPollEvent;
    out.Close        = &FwdClose;
}
}
