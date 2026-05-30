#pragma once
#include <atomic>
#include <cstdint>
#include "lib.h"        // LogLevel, TextColor (via LogFormat.h)
#include "MpscRing.h"

inline constexpr int kLogTextMax = 240;

struct LogRecord {
    LogLevel level;
    uint8_t  color;
    uint32_t seq;
    double   t;
    char     text[kLogTextMax];
};

class LogBus {
public:
    static LogBus& Instance();
    void     Push(LogLevel level, TextColor color, const char* text);
    size_t   Drain(LogRecord* out, size_t maxN);
    uint64_t DroppedCount() const { return m_Dropped.load(std::memory_order_relaxed); }
private:
    LogBus() = default;
    static constexpr size_t kRingSize = 8192;
    MpscRing<LogRecord, kRingSize> m_Ring;
    std::atomic<uint64_t> m_Dropped{0};
    std::atomic<uint32_t> m_Seq{0};
};

void ConsoleLogSink(LogLevel level, TextColor color, const char* text);
