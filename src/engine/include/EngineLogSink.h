#pragma once
#include "Engine.h"   // ENGINE_API
#include "lib.h"      // LogSinkFn
ENGINE_API void EngineInstallLogSink(LogSinkFn fn);
