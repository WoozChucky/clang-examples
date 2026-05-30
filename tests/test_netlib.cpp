#include <cstdio>
#include <cstring>

#include "netlib/netlib.h"
#include "netlib/Endpoint.h"
#include "netlib/IoEvent.h"
#include "netlib/IIo.h"

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

int main() {
    test_version();
    test_value_types();

    if (g_Failures == 0) { std::printf("All netlib tests passed.\n"); return 0; }
    std::printf("%d netlib test(s) FAILED.\n", g_Failures);
    return 1;
}
