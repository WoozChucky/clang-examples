#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>

#include "MpscRing.h"
#include "NetBufferPool.h"
#include "NetServices.h"
#include "network/NetSubsystem.h"
#include "network/NetServicesImpl.h"
#include <chrono>
#include <cstring>

void platform_debug_break(const char*, const char*, int, const char*) {}

static int g_Failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); ++g_Failures; } } while (0)

static void test_mpsc_basic() {
    MpscRing<int, 4> r;
    int out = 0;
    CHECK(!r.Dequeue(out), "mpsc: empty dequeue false");
    CHECK(r.Enqueue(1), "mpsc: enqueue 1");
    CHECK(r.Enqueue(2), "mpsc: enqueue 2");
    CHECK(r.Enqueue(3), "mpsc: enqueue 3");
    CHECK(r.Enqueue(4), "mpsc: enqueue 4 (full ring of 4 holds 4)");
    CHECK(!r.Enqueue(5), "mpsc: enqueue 5 fails (full)");
    CHECK(r.Dequeue(out) && out == 1, "mpsc: dequeue 1");
    CHECK(r.Dequeue(out) && out == 2, "mpsc: dequeue 2");
}

static void test_mpsc_concurrent() {
    constexpr int K = 8, M = 10000;
    MpscRing<int, 1024> r;
    std::atomic<int> producedTotal{0};
    std::vector<std::thread> producers;
    std::atomic<bool> go{false};
    for (int k = 0; k < K; ++k) {
        producers.emplace_back([&]{
            while (!go.load()) {}
            for (int i = 0; i < M; ++i) {
                while (!r.Enqueue(1)) { std::this_thread::yield(); }
                producedTotal.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    go.store(true);
    int received = 0;
    int out = 0;
    while (received < K * M) {
        if (r.Dequeue(out)) received += out;
        else std::this_thread::yield();
    }
    for (auto& t : producers) t.join();
    CHECK(received == K * M, "mpsc: concurrent received exactly K*M");
    CHECK(!r.Dequeue(out), "mpsc: empty after draining all");
}

static void test_pool_basic() {
    NetBufferPool pool(64, 4);   // 4 blocks of 64 bytes
    uint32_t i0 = 0, i1 = 0;
    std::byte* b0 = pool.Acquire(i0);
    CHECK(b0 != nullptr, "pool: acquire 0 non-null");
    std::byte* b1 = pool.Acquire(i1);
    CHECK(b1 != nullptr && i1 != i0, "pool: acquire 1 distinct index");
    const char* msg = "hi";
    std::memcpy(b0, msg, 3);
    CHECK(std::memcmp(pool.Block(i0), "hi", 3) == 0, "pool: Block(i) round-trips bytes");
    pool.Release(i0);
    pool.Release(i1);
    uint32_t i2;
    CHECK(pool.Acquire(i2) != nullptr, "pool: reacquire after release");
    pool.Release(i2);
}

static void test_pool_exhaustion() {
    NetBufferPool pool(32, 2);
    uint32_t a, b, c;
    CHECK(pool.Acquire(a) != nullptr, "pool: acq 1");
    CHECK(pool.Acquire(b) != nullptr, "pool: acq 2");
    CHECK(pool.Acquire(c) == nullptr, "pool: acq 3 exhausted -> null");
    pool.Release(a); pool.Release(b);
}

static void test_pool_concurrent() {
    NetBufferPool pool(64, 256);
    constexpr int K = 8, iters = 20000;
    std::vector<std::thread> ts;
    std::atomic<bool> go{false};
    for (int k = 0; k < K; ++k) ts.emplace_back([&]{
        while (!go.load()) {}
        for (int i = 0; i < iters; ++i) {
            uint32_t idx;
            std::byte* p = pool.Acquire(idx);
            if (p) { p[0] = std::byte{1}; pool.Release(idx); }
        }
    });
    go.store(true);
    for (auto& t : ts) t.join();
    std::vector<uint32_t> held;
    for (int i = 0; i < 256; ++i) { uint32_t idx; if (pool.Acquire(idx)) held.push_back(idx); }
    CHECK(held.size() == 256, "pool: all blocks free after concurrent churn (no leak/double-free)");
    for (uint32_t idx : held) pool.Release(idx);
}

static void test_net_types() {
    netlib::Endpoint ep{ "127.0.0.1", 9000 };
    NetServerConfig sc{}; sc.bind = ep;
    CHECK(sc.bind.port == 9000, "types: NetServerConfig.bind");
    CHECK(sc.gameResident == false, "types: gameResident defaults false");

    NetEvent ev{};
    ev.kind = NetEventKind::Message;
    ev.opcode = 7;
    CHECK(ev.kind == NetEventKind::Message && ev.opcode == 7, "types: NetEvent fields");
    CHECK(NetHandle::Invalid == NetHandle{0}, "types: NetHandle::Invalid is 0");
}

template <typename Pred>
static bool net_wait_until(Pred pred, int timeoutMs = 4000) {
    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    while (!pred()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count() > timeoutMs)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

static std::vector<uint8_t> u8(const char* s) {
    std::vector<uint8_t> v; for (const char* p = s; *p; ++p) v.push_back((uint8_t)*p); return v;
}

static void test_netsub_roundtrip() {
    NetServices net{};
    NetServicesImpl::Init(net);
    NetSubsystem::Instance().Init();

    NetServerConfig sc{}; sc.bind = netlib::Endpoint{ "127.0.0.1", 0 };
    NetHandle srv = net.CreateServer(&netlib::MakeTcpServer, sc);
    CHECK(srv != NetHandle::Invalid, "netsub: server created");
    uint16_t port = net.BoundPort(srv);
    CHECK(port != 0, "netsub: server bound port");

    NetClientConfig cc{}; cc.target = netlib::Endpoint{ "127.0.0.1", port };
    NetHandle cli = net.CreateClient(&netlib::MakeTcpClient, cc);
    CHECK(cli != NetHandle::Invalid, "netsub: client created");

    int serverConns = 0; bool gotClientMsg = false; NetConnId serverConn = kNetConnInvalid;
    auto pump = [&]{
        NetEvent ev{};
        while (net.PollEvent(&ev)) {
            if (ev.adapter == srv && ev.kind == NetEventKind::Connected) { ++serverConns; serverConn = ev.conn; }
            if (ev.adapter == srv && ev.kind == NetEventKind::Message) {
                if (ev.opcode == 42 && ev.len == 5 && std::memcmp(ev.payload, "hello", 5) == 0) gotClientMsg = true;
            }
        }
    };
    CHECK(net_wait_until([&]{ pump(); return serverConns == 1; }), "netsub: server saw a connection");

    auto msg = u8("hello");
    CHECK(net.Send(cli, kNetConnInvalid, 42, msg.data(), msg.size()), "netsub: client send");
    CHECK(net_wait_until([&]{ pump(); return gotClientMsg; }), "netsub: server received opcode+payload");

    bool gotReply = false;
    auto reply = u8("yo");
    net.Send(srv, serverConn, 7, reply.data(), reply.size());
    CHECK(net_wait_until([&]{
        NetEvent ev{};
        while (net.PollEvent(&ev)) if (ev.adapter == cli && ev.kind == NetEventKind::Message
                                        && ev.opcode == 7 && ev.len == 2 && std::memcmp(ev.payload, "yo", 2) == 0) gotReply = true;
        return gotReply;
    }), "netsub: client received server reply");

    net.Close(cli);
    net.Close(srv);
    NetSubsystem::Instance().Shutdown();
}

static void test_release_game_resident() {
    NetServices net{}; NetServicesImpl::Init(net);
    NetSubsystem::Instance().Init();

    NetServerConfig resident{}; resident.bind = netlib::Endpoint{ "127.0.0.1", 0 }; resident.gameResident = true;
    NetServerConfig stable{};   stable.bind   = netlib::Endpoint{ "127.0.0.1", 0 }; stable.gameResident = false;
    NetHandle hr = net.CreateServer(&netlib::MakeTcpServer, resident);
    NetHandle hs = net.CreateServer(&netlib::MakeTcpServer, stable);
    CHECK(hr != NetHandle::Invalid && hs != NetHandle::Invalid, "teardown: both created");

    NetSubsystem::Instance().ReleaseGameResidentConnections();

    CHECK(net.BoundPort(hr) == 0, "teardown: game-resident adapter released");
    CHECK(net.BoundPort(hs) != 0, "teardown: stable adapter survives");

    NetSubsystem::Instance().Shutdown();
}

// Mimics the application-close path: a connected server+client pair torn down by
// a single Shutdown() WITHOUT individually Close()-ing first (the round-trip test
// Closes each before Shutdown; the editor/runtime does NOT — it goes straight to
// NetSubsystem::Shutdown with both adapters live + connected to each other).
static void test_netsub_shutdown_while_connected() {
    NetServices net{}; NetServicesImpl::Init(net);
    NetSubsystem::Instance().Init();

    NetServerConfig sc{}; sc.bind = netlib::Endpoint{ "127.0.0.1", 0 }; sc.gameResident = true;
    NetHandle srv = net.CreateServer(&netlib::MakeTcpServer, sc);
    CHECK(srv != NetHandle::Invalid, "shutdown-live: server created");
    uint16_t port = net.BoundPort(srv);
    NetClientConfig cc{}; cc.target = netlib::Endpoint{ "127.0.0.1", port }; cc.gameResident = true;
    NetHandle cli = net.CreateClient(&netlib::MakeTcpClient, cc);
    CHECK(cli != NetHandle::Invalid, "shutdown-live: client created");

    int conns = 0;
    net_wait_until([&]{ NetEvent ev{}; while (net.PollEvent(&ev)) if (ev.kind == NetEventKind::Connected) ++conns; return conns >= 2; });
    auto m = u8("hi");
    net.Send(cli, kNetConnInvalid, 1, m.data(), m.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Tear down with BOTH adapters still live + connected — the app-close path.
    NetSubsystem::Instance().Shutdown();
    CHECK(true, "shutdown-live: Shutdown() of a connected pair did not crash/hang");
}

// Faithful repro of the editor's NetDemoSystem at close: continuous BIDIRECTIONAL
// traffic (client pings, server echoes) so both adapters have in-flight ops, then
// straight to Shutdown() WITHOUT draining (the app destroys the system then Shutdowns).
static void test_netsub_traffic_then_shutdown() {
    NetServices net{}; NetServicesImpl::Init(net);
    NetSubsystem::Instance().Init();

    NetServerConfig sc{}; sc.bind = netlib::Endpoint{ "127.0.0.1", 0 }; sc.gameResident = true;
    NetHandle srv = net.CreateServer(&netlib::MakeTcpServer, sc);
    uint16_t port = net.BoundPort(srv);
    NetClientConfig cc{}; cc.target = netlib::Endpoint{ "127.0.0.1", port }; cc.gameResident = true;
    NetHandle cli = net.CreateClient(&netlib::MakeTcpClient, cc);
    CHECK(srv != NetHandle::Invalid && cli != NetHandle::Invalid, "traffic: created");

    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    auto m = u8("ping");
    while (std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count() < 400) {
        NetEvent ev{};
        while (net.PollEvent(&ev)) {
            if (ev.adapter == srv && ev.kind == NetEventKind::Message)
                net.Send(srv, ev.conn, 2, ev.payload, ev.len);   // server echoes
        }
        net.Send(cli, kNetConnInvalid, 1, m.data(), m.size());   // client pings hard (no 2s gap)
    }
    // Close path: straight to Shutdown with traffic in flight, no final drain.
    NetSubsystem::Instance().Shutdown();
    CHECK(true, "traffic-then-shutdown: did not crash");
}

int main() {
    test_net_types();
    test_mpsc_basic();
    test_mpsc_concurrent();
    test_pool_basic();
    test_pool_exhaustion();
    test_pool_concurrent();
    test_netsub_roundtrip();
    test_release_game_resident();
    test_netsub_shutdown_while_connected();
    test_netsub_traffic_then_shutdown();
    if (g_Failures == 0) { std::printf("All net tests passed.\n"); return 0; }
    std::printf("%d net test(s) FAILED.\n", g_Failures);
    return 1;
}
