#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "netlib/netlib.h"
#include "netlib/Endpoint.h"
#include "netlib/IoEvent.h"
#include "netlib/IIo.h"
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

static std::vector<std::byte> bytes_of(const char* s) {
    std::vector<std::byte> v;
    for (const char* p = s; *p; ++p) v.push_back(static_cast<std::byte>(*p));
    return v;
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
    std::atomic<int> connected{0}, disconnected{0};
    // When set, the sink echoes every received Message back on its connection,
    // exercising Send()/PostSend() concurrently with disconnects.
    netlib::IIoServer* echoServer = nullptr;

    void OnIoEvent(const netlib::IoEvent& ev) override {
        if (ev.kind == netlib::IoEvent::Kind::Message && echoServer)
            echoServer->Send(ev.conn, ev.payload);   // Send copies the borrowed span
        std::scoped_lock lk(mx);
        kinds.push_back(ev.kind);
        if (ev.kind == netlib::IoEvent::Kind::Connected)    ++connected;
        if (ev.kind == netlib::IoEvent::Kind::Disconnected) ++disconnected;
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

    auto msg = bytes_of("ping");
    pair.client->Send(msg);
    CHECK(serverSink.msgCount() == 1, "inmem: server received 1 msg");
    CHECK(serverSink.messages[0] == bytes_of("ping"), "inmem: server payload correct");

    auto reply = bytes_of("pong");
    // server replies to the single client connection (ConnId 1 by convention).
    pair.server->Send(netlib::ConnId{1}, reply);
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

int main() {
    test_version();
    test_value_types();
    test_framer();
    test_inmemory();
    test_tcp_server();
    test_tcp_stress();
    test_tcp_stress_echo();

    if (g_Failures == 0) { std::printf("All netlib tests passed.\n"); return 0; }
    std::printf("%d netlib test(s) FAILED.\n", g_Failures);
    return 1;
}
