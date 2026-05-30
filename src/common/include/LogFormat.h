#pragma once
#include <cstdio>   // std::snprintf
#include <string>

// ANSI terminal colors for logging. (Moved here from lib.h so the formatter is small + testable.)
enum TextColor {
    TEXT_COLOR_BLACK, TEXT_COLOR_RED, TEXT_COLOR_GREEN, TEXT_COLOR_YELLOW,
    TEXT_COLOR_BLUE, TEXT_COLOR_MAGENTA, TEXT_COLOR_CYAN, TEXT_COLOR_WHITE,
    TEXT_COLOR_BRIGHT_BLACK, TEXT_COLOR_BRIGHT_RED, TEXT_COLOR_BRIGHT_GREEN,
    TEXT_COLOR_BRIGHT_YELLOW, TEXT_COLOR_BRIGHT_BLUE, TEXT_COLOR_BRIGHT_MAGENTA,
    TEXT_COLOR_BRIGHT_CYAN, TEXT_COLOR_BRIGHT_WHITE, TEXT_COLOR_COUNT
};

inline const char* text_color_code(TextColor c) {
    static const char* kTable[TEXT_COLOR_COUNT] = {
        "\x1b[30m","\x1b[31m","\x1b[32m","\x1b[33m","\x1b[34m","\x1b[35m","\x1b[36m","\x1b[37m",
        "\x1b[90m","\x1b[91m","\x1b[92m","\x1b[93m","\x1b[94m","\x1b[95m","\x1b[96m","\x1b[97m",
    };
    return (c >= 0 && c < TEXT_COLOR_COUNT) ? kTable[c] : "";
}

// Formats the user message + args ONCE (printf-style) into a bounded stack buffer, then wraps it:
// "<color> <prefix> <body>\033[0m". The reset (\033[0m) is part of this literal, never a passed arg
// (that was the SM_WARN/SM_ERROR off-by-one bug). Pure: no globals, no I/O -> unit-testable.
template <typename... Args>
std::string format_log_line(const char* prefix, const char* msg, TextColor color, Args... args) {
    char body[8192];
    std::snprintf(body, sizeof(body), msg, args...);
    std::string out;
    out += text_color_code(color);
    out += ' ';
    out += prefix;
    out += ' ';
    out += body;
    out += "\033[0m";
    return out;
}

// Like format_log_line but PLAIN: "<prefix> <body>" with no ANSI color codes / reset.
// Used by the log sink hook so editor-side consumers get clean text.
template <typename... Args>
std::string format_log_body(const char* prefix, const char* msg, Args... args) {
    char body[8192];
    std::snprintf(body, sizeof(body), msg, args...);
    std::string out;
    out += prefix;
    out += ' ';
    out += body;
    return out;
}
