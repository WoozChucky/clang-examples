#include "LogBus.h"
#include <chrono>
#include <cstring>

LogBus& LogBus::Instance() {
    static LogBus s_Instance;
    return s_Instance;
}

void LogBus::Push(LogLevel level, TextColor color, const char* text) {
    LogRecord r{};
    r.level = level;
    r.color = static_cast<uint8_t>(color);
    // seq is a near-monotonic display/tiebreak field, NOT authoritative ordering: it is grabbed
    // before Enqueue, so two concurrent producers can commit to the ring out of seq order. The
    // ring (dequeue) order is the true arrival order. (Relaxed: no data published via seq.)
    r.seq   = m_Seq.fetch_add(1, std::memory_order_relaxed);
    r.t     = std::chrono::duration<double>(
                  std::chrono::steady_clock::now().time_since_epoch()).count();

    const size_t cap = kLogTextMax - 1;
    const size_t len = text ? std::strlen(text) : 0;
    const size_t n   = (len > cap) ? cap : len;
    if (n) std::memcpy(r.text, text, n);
    if (len > cap && cap >= 3) {
        std::memcpy(r.text + cap - 3, "...", 3);   // mark truncation within the copied region
    }
    r.text[n] = '\0';

    if (!m_Ring.Enqueue(r)) {
        m_Dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

size_t LogBus::Drain(LogRecord* out, size_t maxN) {
    size_t n = 0;
    LogRecord r;
    while (n < maxN && m_Ring.Dequeue(r)) out[n++] = r;
    return n;
}

void ConsoleLogSink(LogLevel level, TextColor color, const char* text) {
    LogBus::Instance().Push(level, color, text);
}
