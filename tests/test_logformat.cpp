#include <cstdio>
#include <cstddef>
#include <string>

#include "LogFormat.h"
#include "lib.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static void T00_plain_no_args()
{
    EXPECT(format_log_line("TRACE:", "hello", TEXT_COLOR_GREEN)
           == std::string("\x1b[32m TRACE: hello\033[0m"));
}

static void T01_single_int_arg_not_shifted()
{
    // The whole point: the int arg fills %d (it is NOT shifted by a phantom reset arg).
    EXPECT(format_log_line("WARN:", "n=%d", TEXT_COLOR_YELLOW, 5)
           == std::string("\x1b[33m WARN: n=5\033[0m"));
}

static void T02_multiple_mixed_args_in_order()
{
    EXPECT(format_log_line("ERROR:", "%s=%zu", TEXT_COLOR_RED, "x", static_cast<std::size_t>(42))
           == std::string("\x1b[31m ERROR: x=42\033[0m"));
}

static void T03_literal_without_specifiers()
{
    EXPECT(format_log_line("TRACE:", "plain text", TEXT_COLOR_GREEN)
           == std::string("\x1b[32m TRACE: plain text\033[0m"));
}

// format_log_body produces the PLAIN "<prefix> <body>" with NO ANSI escape codes.
static void T04_body_no_ansi()
{
    const std::string b = format_log_body("WARN:", "n=%d", 7);
    EXPECT(b == std::string("WARN: n=7"));
    EXPECT(b.find('\x1b') == std::string::npos);
    EXPECT(b.find("\033[0m") == std::string::npos);
}

static int   g_SinkCalls = 0;
static LogLevel g_SinkLevel = LogLevel::Trace;
static std::string g_SinkText;
static void TestSink(LogLevel lvl, TextColor /*c*/, const char* text) {
    ++g_SinkCalls; g_SinkLevel = lvl; g_SinkText = text;
}
static void T05_sink_hook()
{
    g_SinkCalls = 0;
    sm_set_log_sink(&TestSink);
    SM_WARN("hello %d", 42);
    EXPECT(g_SinkCalls == 1);
    EXPECT(g_SinkLevel == LogLevel::Warn);
    EXPECT(g_SinkText == std::string("WARN: hello 42"));
    sm_set_log_sink(nullptr);
    SM_ERROR("ignored");
    EXPECT(g_SinkCalls == 1);
}

int main()
{
    T00_plain_no_args();
    T01_single_int_arg_not_shifted();
    T02_multiple_mixed_args_in_order();
    T03_literal_without_specifiers();
    T04_body_no_ansi();
    T05_sink_hook();

    if (g_Failures == 0) { std::printf("All log format tests passed.\n"); return 0; }
    std::printf("%d log format test(s) FAILED.\n", g_Failures);
    return 1;
}
