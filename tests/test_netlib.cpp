#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "netlib/netlib.h"
#include "netlib/Endpoint.h"
#include "netlib/IoEvent.h"
#include "netlib/IIo.h"
#include "netlib/OwnedBuffer.h"
#include "Framer.h"   // internal; test include dir covers src/netlib/src

// Per-module SM_ASSERT backend (declared in lib.h, each exe provides its own).
// netlib itself avoids SM_ASSERT, but lib.h's templates may reference this; define
// it here so the test exe always links. Mirrors src/runtime/src/main.cpp.
void platform_debug_break(const char*, const char*, int, const char*) {}

static int g_Failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); ++g_Failures; } } while (0)

static void test_version() {
    const char* v = netlib::Version();
    CHECK(v != nullptr, "Version() returns non-null");
    CHECK(std::strncmp(v, "netlib", 6) == 0, "Version() starts with 'netlib'");
}

static void test_value_types() {
    netlib::Endpoint ep{ "127.0.0.1", 7777 };
    CHECK(ep.port == 7777, "Endpoint.port set");
    CHECK(ep.host == "127.0.0.1", "Endpoint.host set");

    netlib::ConnConfig cfg{};
    CHECK(cfg.noDelay == true, "ConnConfig.noDelay defaults true (TCP_NODELAY on)");

    netlib::IoEvent ev{};
    ev.kind = netlib::IoEvent::Kind::Message;
    CHECK(ev.kind == netlib::IoEvent::Kind::Message, "IoEvent.kind assignable");
}

static void test_owned_buffer() {
    using netlib::OwnedBuffer;

    // (a) MakeHeapBuffer: payload writable, sizes correct.
    {
        OwnedBuffer b = netlib::MakeHeapBuffer(5);
        CHECK(b.Valid(), "ownedbuf: heap buffer valid");
        CHECK(b.PayloadLen() == 5, "ownedbuf: payload len 5");
        CHECK(b.TotalSize() == 9, "ownedbuf: total = head(4)+5");
        CHECK(b.Payload() == b.Data() + 4, "ownedbuf: payload past 4-byte head");
        for (int i = 0; i < 5; ++i) b.Payload()[i] = static_cast<std::byte>('a' + i);
        CHECK(b.Payload()[0] == static_cast<std::byte>('a'), "ownedbuf: payload writable");
    }

    // (b) Deleter runs exactly once on destruction; move transfers ownership.
    {
        static int freed = 0; freed = 0;
        std::byte storage[8] = {};
        auto del = [](void* ctx, std::byte*) noexcept { ++*static_cast<int*>(ctx); };
        {
            OwnedBuffer a(storage, /*payloadLen*/ 4, &freed, del);
            OwnedBuffer moved = std::move(a);
            CHECK(!a.Valid(), "ownedbuf: moved-from invalid");
            CHECK(moved.Valid() && moved.PayloadLen() == 4, "ownedbuf: moved-to owns");
            CHECK(freed == 0, "ownedbuf: deleter not run while alive");
        }
        CHECK(freed == 1, "ownedbuf: deleter ran exactly once after scope");
    }
}

static std::vector<std::byte> bytes_of(const char* s) {
    std::vector<std::byte> v;
    for (const char* p = s; *p; ++p) v.push_back(static_cast<std::byte>(*p));
    return v;
}

// Build a heap OwnedBuffer whose payload is a copy of `s` (for tests that send bytes).
static netlib::OwnedBuffer owned_of(const char* s) {
    auto v = bytes_of(s);
    netlib::OwnedBuffer b = netlib::MakeHeapBuffer(static_cast<uint32_t>(v.size()));
    for (size_t i = 0; i < v.size(); ++i) b.Payload()[i] = v[i];
    return b;
}
static netlib::OwnedBuffer owned_of(std::span<const std::byte> s) {
    netlib::OwnedBuffer b = netlib::MakeHeapBuffer(static_cast<uint32_t>(s.size()));
    for (size_t i = 0; i < s.size(); ++i) b.Payload()[i] = s[i];
    return b;
}

// Build a wire frame: [uint32 LE length][payload].
static std::vector<std::byte> wire_frame(const char* payload) {
    auto p = bytes_of(payload);
    std::vector<std::byte> out;
    const uint32_t len = static_cast<uint32_t>(p.size());
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((len >> (8 * i)) & 0xff));
    out.insert(out.end(), p.begin(), p.end());
    return out;
}

static void test_framer() {
    using netlib::detail::Framer;

    // (a) One frame fed one byte at a time -> exactly one emitted frame.
    {
        Framer f(1u << 24);
        auto wire = wire_frame("hello");
        std::vector<std::vector<std::byte>> got;
        bool ok = true;
        for (std::byte b : wire) {
            ok = ok && f.Push(std::span(&b, 1), [&](std::span<const std::byte> frame) {
                got.emplace_back(frame.begin(), frame.end());
            });
        }
        CHECK(ok, "framer: drip feed no error");
        CHECK(got.size() == 1, "framer: drip feed -> 1 frame");
        CHECK(got.size() == 1 && got[0] == bytes_of("hello"), "framer: drip payload correct");
    }

    // (b) Two frames coalesced in one buffer -> two emitted frames.
    {
        Framer f(1u << 24);
        auto w1 = wire_frame("aa");
        auto w2 = wire_frame("bbbb");
        std::vector<std::byte> both = w1;
        both.insert(both.end(), w2.begin(), w2.end());
        std::vector<std::vector<std::byte>> got;
        bool ok = f.Push(both, [&](std::span<const std::byte> fr) { got.emplace_back(fr.begin(), fr.end()); });
        CHECK(ok, "framer: coalesced no error");
        CHECK(got.size() == 2, "framer: coalesced -> 2 frames");
        CHECK(got.size() == 2 && got[0] == bytes_of("aa") && got[1] == bytes_of("bbbb"),
              "framer: coalesced payloads correct");
    }

    // (c) Oversize length prefix -> Push returns false (caller disconnects), no huge alloc.
    {
        Framer f(8);  // max 8-byte frames
        std::vector<std::byte> hdr;
        const uint32_t huge = 1000000;
        for (int i = 0; i < 4; ++i) hdr.push_back(static_cast<std::byte>((huge >> (8 * i)) & 0xff));
        bool ok = f.Push(hdr, [](std::span<const std::byte>) {});
        CHECK(!ok, "framer: oversize length -> Push returns false");
    }
}

// Thread-safe recording sink (reused for socket tests later).
struct RecordingSink : netlib::IIoSink {
    std::mutex mx;
    std::vector<netlib::IoEvent::Kind> kinds;
    std::vector<std::vector<std::byte>> messages;
    std::atomic<int> connected{0}, disconnected{0}, errors{0};
    // When set, the sink echoes every received Message back on its connection,
    // exercising Send()/PostSend() concurrently with disconnects.
    netlib::IIoServer* echoServer = nullptr;

    void OnIoEvent(const netlib::IoEvent& ev) override {
        if (ev.kind == netlib::IoEvent::Kind::Message && echoServer)
            echoServer->Send(ev.conn, owned_of(ev.payload));   // echo via OwnedBuffer
        std::scoped_lock lk(mx);
        kinds.push_back(ev.kind);
        if (ev.kind == netlib::IoEvent::Kind::Connected)    ++connected;
        if (ev.kind == netlib::IoEvent::Kind::Disconnected) ++disconnected;
        if (ev.kind == netlib::IoEvent::Kind::Error)        ++errors;
        if (ev.kind == netlib::IoEvent::Kind::Message)
            messages.emplace_back(ev.payload.begin(), ev.payload.end());
    }
    size_t msgCount() { std::scoped_lock lk(mx); return messages.size(); }
};

static void test_inmemory() {
    auto pair = netlib::MakeInMemoryPair();
    RecordingSink serverSink, clientSink;

    netlib::ConnConfig cfg{};
    CHECK(pair.server->Start(netlib::Endpoint{}, cfg, &serverSink), "inmem: server Start");
    CHECK(pair.client->Start(netlib::Endpoint{}, cfg, &clientSink), "inmem: client Start");
    CHECK(serverSink.connected == 1, "inmem: server saw Connected");
    CHECK(clientSink.connected == 1, "inmem: client saw Connected");

    pair.client->Send(owned_of("ping"));
    CHECK(serverSink.msgCount() == 1, "inmem: server received 1 msg");
    CHECK(serverSink.messages[0] == bytes_of("ping"), "inmem: server payload correct");

    // server replies to the single client connection (ConnId 1 by convention).
    pair.server->Send(netlib::ConnId{1}, owned_of("pong"));
    CHECK(clientSink.msgCount() == 1, "inmem: client received reply");
    CHECK(clientSink.messages[0] == bytes_of("pong"), "inmem: client reply correct");

    pair.client->Stop();
    CHECK(serverSink.disconnected == 1, "inmem: server saw Disconnected after client Stop");
}

// Wait until `pred()` or timeout. Returns pred() final value. Avoids sleep-and-hope.
template <typename Pred>
static bool wait_until(Pred pred, int timeoutMs = 3000) {
    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    while (!pred()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count() > timeoutMs)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// Connect a raw blocking socket to 127.0.0.1:port, return the socket or INVALID_SOCKET.
static SOCKET raw_connect(uint16_t port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::closesocket(s); return INVALID_SOCKET;
    }
    return s;
}

static void send_framed(SOCKET s, const char* payload) {
    auto p = bytes_of(payload);
    uint32_t len = static_cast<uint32_t>(p.size());
    ::send(s, reinterpret_cast<const char*>(&len), 4, 0);   // x64 LE
    ::send(s, reinterpret_cast<const char*>(p.data()), static_cast<int>(p.size()), 0);
}

static void test_tcp_server() {
    // The raw-socket helpers need WSAStartup in THIS exe too (netlib has its own).
    WSADATA wsa{}; WSAStartup(MAKEWORD(2,2), &wsa);

    auto server = netlib::MakeTcpServer();
    RecordingSink sink;
    netlib::ConnConfig cfg{};
    // Port 0 => OS picks an ephemeral port; we read it back via BoundPort().
    netlib::Endpoint bind{ "127.0.0.1", 0 };
    CHECK(server->Start(bind, cfg, &sink), "tcpsrv: Start");

    uint16_t port = server->BoundPort();
    CHECK(port != 0, "tcpsrv: bound to a real port");

    SOCKET c = raw_connect(port);
    CHECK(c != INVALID_SOCKET, "tcpsrv: raw client connected");
    CHECK(wait_until([&]{ return sink.connected.load() == 1; }), "tcpsrv: server saw Connected");

    send_framed(c, "hello-server");
    CHECK(wait_until([&]{ return sink.msgCount() == 1; }), "tcpsrv: server received frame");
    {
        std::scoped_lock lk(sink.mx);
        CHECK(sink.messages.size() == 1 && sink.messages[0] == bytes_of("hello-server"),
              "tcpsrv: server payload correct");
    }

    ::closesocket(c);
    CHECK(wait_until([&]{ return sink.disconnected.load() == 1; }), "tcpsrv: server saw Disconnected");

    server->Stop();
    WSACleanup();
}

// Concurrency stress: N raw clients connect, send 3 frames, then abruptly close.
// Exercises the recv-error + in-flight-op reap races in IocpCore's lifetime model.
// The real assertion is "we got here without crashing/hanging" + balanced conn counts.
static void test_tcp_stress() {
    WSADATA wsa{}; WSAStartup(MAKEWORD(2,2), &wsa);

    auto server = netlib::MakeTcpServer();
    RecordingSink sink;
    netlib::ConnConfig cfg{};
    netlib::Endpoint bind{ "127.0.0.1", 0 };
    CHECK(server->Start(bind, cfg, &sink), "stress: Start");
    uint16_t port = server->BoundPort();
    CHECK(port != 0, "stress: bound to a real port");

    constexpr int N = 20;
    std::vector<std::thread> clients;
    clients.reserve(N);
    for (int i = 0; i < N; ++i) {
        clients.emplace_back([port] {
            SOCKET s = raw_connect(port);
            if (s == INVALID_SOCKET) return;
            send_framed(s, "msg-one");
            send_framed(s, "msg-two");
            send_framed(s, "msg-three");
            ::closesocket(s);   // abrupt close — races the in-flight recv/op reap
        });
    }
    for (auto& t : clients) t.join();

    // Every connection must open and close exactly once — balanced, no crash.
    CHECK(wait_until([&]{ return sink.connected.load() == N && sink.disconnected.load() == N; }, 5000),
          "stress: all connections opened and closed");
    CHECK(sink.connected.load() == N, "stress: connected == 20");
    CHECK(sink.disconnected.load() == N, "stress: disconnected == 20");
    // Message count may be < 60 if a close races a partial send; don't over-constrain.
    CHECK(sink.msgCount() <= static_cast<size_t>(N * 3), "stress: msgCount <= 60");

    server->Stop();
    WSACleanup();
}

// Concurrency stress with server-side echo: N clients connect, send 3 frames,
// then abruptly close. The sink echoes every received Message back via
// server->Send(), so PostSend()/the Conn::sock path runs concurrently with the
// abrupt client disconnects (echoing to a connection that may already be closing).
// Real assertion: balanced conn counts, no crash. Don't constrain echo receipt.
static void test_tcp_stress_echo() {
    WSADATA wsa{}; WSAStartup(MAKEWORD(2,2), &wsa);

    auto server = netlib::MakeTcpServer();
    RecordingSink sink;
    sink.echoServer = server.get();   // sink echoes back through the server
    netlib::ConnConfig cfg{};
    netlib::Endpoint bind{ "127.0.0.1", 0 };
    CHECK(server->Start(bind, cfg, &sink), "echo: Start");
    uint16_t port = server->BoundPort();
    CHECK(port != 0, "echo: bound to a real port");

    constexpr int N = 20;
    std::vector<std::thread> clients;
    clients.reserve(N);
    for (int i = 0; i < N; ++i) {
        clients.emplace_back([port, i] {
            SOCKET s = raw_connect(port);
            if (s == INVALID_SOCKET) return;
            send_framed(s, "echo-one");
            send_framed(s, "echo-two");
            send_framed(s, "echo-three");
            // Some clients close immediately (racing the server's echo back to a
            // closing connection); others linger briefly to let an echo arrive.
            if (i % 2 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(3));
            ::closesocket(s);
        });
    }
    for (auto& t : clients) t.join();

    // Every connection must open and close exactly once — balanced, no crash.
    CHECK(wait_until([&]{ return sink.connected.load() == N && sink.disconnected.load() == N; }, 5000),
          "echo: all connections opened and closed");
    CHECK(sink.connected.load() == N, "echo: connected == 20");
    CHECK(sink.disconnected.load() == N, "echo: disconnected == 20");

    server->Stop();
    WSACleanup();
}

static void test_tcp_roundtrip() {
    auto server = netlib::MakeTcpServer();
    RecordingSink srvSink;
    netlib::ConnConfig cfg{};
    CHECK(server->Start(netlib::Endpoint{ "127.0.0.1", 0 }, cfg, &srvSink), "rt: server Start");
    uint16_t port = server->BoundPort();

    auto client = netlib::MakeTcpClient();
    RecordingSink cliSink;
    CHECK(client->Start(netlib::Endpoint{ "127.0.0.1", port }, cfg, &cliSink), "rt: client Start");
    CHECK(wait_until([&]{ return cliSink.connected.load() == 1; }), "rt: client Connected");
    CHECK(wait_until([&]{ return srvSink.connected.load() == 1; }), "rt: server Connected");

    client->Send(owned_of("from-client"));
    CHECK(wait_until([&]{ return srvSink.msgCount() == 1; }), "rt: server got client msg");
    { std::scoped_lock lk(srvSink.mx);
      CHECK(srvSink.messages[0] == bytes_of("from-client"), "rt: server payload"); }

    // Reply to that connection (its ConnId is the first registered = 1).
    server->Send(netlib::ConnId{1}, owned_of("from-server"));
    CHECK(wait_until([&]{ return cliSink.msgCount() == 1; }), "rt: client got server reply");
    { std::scoped_lock lk(cliSink.mx);
      CHECK(cliSink.messages[0] == bytes_of("from-server"), "rt: client payload"); }

    client->Stop();
    server->Stop();
}

static void test_tcp_multiconn() {
    auto server = netlib::MakeTcpServer();
    RecordingSink srvSink;
    netlib::ConnConfig cfg{};
    server->Start(netlib::Endpoint{ "127.0.0.1", 0 }, cfg, &srvSink);
    uint16_t port = server->BoundPort();

    auto c1 = netlib::MakeTcpClient(); RecordingSink s1;
    auto c2 = netlib::MakeTcpClient(); RecordingSink s2;
    c1->Start(netlib::Endpoint{ "127.0.0.1", port }, cfg, &s1);
    c2->Start(netlib::Endpoint{ "127.0.0.1", port }, cfg, &s2);
    CHECK(wait_until([&]{ return srvSink.connected.load() == 2; }), "multi: server saw 2 Connected");

    c1->Send(owned_of("one"));
    c2->Send(owned_of("two"));
    CHECK(wait_until([&]{ return srvSink.msgCount() == 2; }), "multi: server got 2 msgs");
    {
        std::scoped_lock lk(srvSink.mx);
        bool sawOne = false, sawTwo = false;
        for (auto& m : srvSink.messages) { if (m == bytes_of("one")) sawOne = true; if (m == bytes_of("two")) sawTwo = true; }
        CHECK(sawOne && sawTwo, "multi: both payloads attributed");
    }

    c1->Stop(); c2->Stop(); server->Stop();
}

// A genuine connect failure (nothing listening on the target port) must notify
// the sink with an Error event so a caller can drive connect-retry, and must
// never report Connected.
static void test_tcp_connect_failure() {
    netlib::ConnConfig cfg{};

    // Bind a server to an ephemeral port, read it back, then Stop() to free it.
    // An immediately-following loopback connect to that freed port is refused.
    auto tmp = netlib::MakeTcpServer();
    RecordingSink t;
    tmp->Start(netlib::Endpoint{ "127.0.0.1", 0 }, cfg, &t);
    uint16_t deadPort = tmp->BoundPort();
    CHECK(deadPort != 0, "connfail: obtained a real (now-freed) port");
    tmp->Stop();

    auto client = netlib::MakeTcpClient();
    RecordingSink cs;
    CHECK(client->Start(netlib::Endpoint{ "127.0.0.1", deadPort }, cfg, &cs),
          "connfail: Start returns true (initiation ok)");
    CHECK(wait_until([&]{ return cs.errors.load() == 1; }),
          "connfail: sink notified of connect failure");
    CHECK(cs.connected.load() == 0, "connfail: never reported Connected");

    client->Stop();
}

// Clean shutdown: after Stop() returns, all threads (accept + workers + client)
// must be joined, so no further sink events can fire. We snapshot the recorded
// event-kind count immediately after Stop(), wait a beat, and assert it's stable.
static void test_stop_is_clean() {
    auto server = netlib::MakeTcpServer();
    RecordingSink srvSink;
    netlib::ConnConfig cfg{};
    server->Start(netlib::Endpoint{ "127.0.0.1", 0 }, cfg, &srvSink);
    uint16_t port = server->BoundPort();

    auto client = netlib::MakeTcpClient(); RecordingSink cliSink;
    client->Start(netlib::Endpoint{ "127.0.0.1", port }, cfg, &cliSink);
    CHECK(wait_until([&]{ return cliSink.connected.load() == 1; }), "stop: connected");

    client->Stop();          // must join its threads
    server->Stop();          // must join accept thread + workers
    const size_t kindsAfter = [&]{ std::scoped_lock lk(srvSink.mx); return srvSink.kinds.size(); }();
    // Give any (incorrectly) lingering thread a moment; count must not grow.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const size_t kindsLater = [&]{ std::scoped_lock lk(srvSink.mx); return srvSink.kinds.size(); }();
    CHECK(kindsAfter == kindsLater, "stop: no events fire after Stop() returns");
}

// Over-the-wire oversize frame: a client sends a length prefix larger than the
// server's configured maxFrameBytes. The framer must reject it and the server
// must disconnect the connection (rather than attempting a huge allocation).
static void test_oversize_frame_disconnects() {
    WSADATA wsa{}; WSAStartup(MAKEWORD(2,2), &wsa);

    auto server = netlib::MakeTcpServer();
    RecordingSink srvSink;
    netlib::ConnConfig cfg{};
    cfg.maxFrameBytes = 16;          // tiny cap
    server->Start(netlib::Endpoint{ "127.0.0.1", 0 }, cfg, &srvSink);
    uint16_t port = server->BoundPort();

    SOCKET c = raw_connect(port);
    CHECK(c != INVALID_SOCKET, "oversize: raw connect");
    CHECK(wait_until([&]{ return srvSink.connected.load() == 1; }), "oversize: server connected");

    uint32_t huge = 1000000;         // > maxFrameBytes
    ::send(c, reinterpret_cast<const char*>(&huge), 4, 0);
    CHECK(wait_until([&]{ return srvSink.disconnected.load() == 1; }), "oversize: server disconnects on bad length");

    ::closesocket(c);
    server->Stop();
    WSACleanup();
}

int main() {
    test_version();
    test_value_types();
    test_owned_buffer();
    test_framer();
    test_inmemory();
    test_tcp_server();
    test_tcp_stress();
    test_tcp_stress_echo();
    test_tcp_roundtrip();
    test_tcp_multiconn();
    test_tcp_connect_failure();
    test_stop_is_clean();
    test_oversize_frame_disconnects();

    if (g_Failures == 0) { std::printf("All netlib tests passed.\n"); return 0; }
    std::printf("%d netlib test(s) FAILED.\n", g_Failures);
    return 1;
}
