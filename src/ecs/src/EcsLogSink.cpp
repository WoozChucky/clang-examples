#include "EcsLogSink.h"
void EcsInstallLogSink(LogSinkFn fn) { sm_set_log_sink(fn); }   // sets ecs.dll's copy
