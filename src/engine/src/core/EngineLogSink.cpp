#include "EngineLogSink.h"
void EngineInstallLogSink(LogSinkFn fn) { sm_set_log_sink(fn); }   // sets Engine.dll's copy
