#pragma once
#include <deque>
#include <string>
#include "LogBus.h"   // LogRecord, LogBus

class ConsolePanel {
public:
    void Draw(bool* open);   // call once per frame
private:
    void DrainNew();
    static constexpr size_t kScrollback = 10000;
    std::deque<LogRecord> m_Lines;
    bool  m_ShowTrace = true;
    bool  m_ShowWarn  = true;
    bool  m_ShowError = true;
    bool  m_AutoScroll = true;
    char  m_Filter[128] = {0};
};
