#pragma once

#include <chrono>

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
static double TimeNowSec() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}