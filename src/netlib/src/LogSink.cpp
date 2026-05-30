#include "netlib/log_sink.h"
namespace netlib { void SetLogSink(LogSinkFn fn) { ::sm_set_log_sink(fn); } }   // sets netlib.dll's copy
