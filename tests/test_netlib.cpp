#include <cstdio>
#include <cstring>
#include <span>
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

int main() {
    test_version();
    test_value_types();
    test_framer();

    if (g_Failures == 0) { std::printf("All netlib tests passed.\n"); return 0; }
    std::printf("%d netlib test(s) FAILED.\n", g_Failures);
    return 1;
}
