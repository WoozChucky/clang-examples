#include "network/NetServicesImpl.h"
#include "network/NetSubsystem.h"

namespace {
NetHandle FwdCreateServer(NetServerFactory f, const NetServerConfig& c) { return NetSubsystem::Instance().CreateServer(f, c); }
NetHandle FwdCreateClient(NetClientFactory f, const NetClientConfig& c) { return NetSubsystem::Instance().CreateClient(f, c); }
uint16_t  FwdBoundPort(NetHandle h) { return NetSubsystem::Instance().BoundPort(h); }
bool      FwdSend(NetHandle h, NetConnId conn, uint16_t op, const uint8_t* d, size_t n) { return NetSubsystem::Instance().Send(h, conn, op, d, n); }
bool      FwdPollEvent(NetEvent* o) { return NetSubsystem::Instance().PollEvent(o); }
void      FwdClose(NetHandle h) { NetSubsystem::Instance().Close(h); }
}

namespace NetServicesImpl {
void Init(NetServices& out) {
    out.CreateServer = &FwdCreateServer;
    out.CreateClient = &FwdCreateClient;
    out.BoundPort    = &FwdBoundPort;
    out.Send         = &FwdSend;
    out.PollEvent    = &FwdPollEvent;
    out.Close        = &FwdClose;
}
}
