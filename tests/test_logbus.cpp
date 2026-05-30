#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "LogBus.h"

static int g_Failures = 0;
#define EXPECT(c) do { if(!(c)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); ++g_Failures; } } while(0)

int main() {
    LogBus& bus = LogBus::Instance();

    bus.Push(LogLevel::Trace, TEXT_COLOR_GREEN,  "first");
    bus.Push(LogLevel::Warn,  TEXT_COLOR_YELLOW, "second");
    bus.Push(LogLevel::Error, TEXT_COLOR_RED,    "third");

    LogRecord out[8];
    size_t n = bus.Drain(out, 8);
    EXPECT(n == 3);
    EXPECT(out[0].level == LogLevel::Trace && std::string(out[0].text) == "first");
    EXPECT(out[1].level == LogLevel::Warn  && std::string(out[1].text) == "second");
    EXPECT(out[2].level == LogLevel::Error && std::string(out[2].text) == "third");
    EXPECT(out[0].seq < out[1].seq && out[1].seq < out[2].seq);

    EXPECT(bus.Drain(out, 8) == 0);

    std::string big(1000, 'x');
    bus.Push(LogLevel::Trace, TEXT_COLOR_GREEN, big.c_str());
    n = bus.Drain(out, 8);
    EXPECT(n == 1);
    EXPECT(std::strlen(out[0].text) == kLogTextMax - 1);

    const uint64_t before = bus.DroppedCount();
    for (int i = 0; i < 9000; ++i) bus.Push(LogLevel::Trace, TEXT_COLOR_GREEN, "flood");
    EXPECT(bus.DroppedCount() > before);
    while (bus.Drain(out, 8) > 0) {}

    if (g_Failures == 0) { std::printf("All logbus tests passed.\n"); return 0; }
    std::printf("%d logbus test(s) FAILED.\n", g_Failures);
    return 1;
}
