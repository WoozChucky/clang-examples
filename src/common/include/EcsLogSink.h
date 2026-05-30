#pragma once
#include "ECS.h"   // ECS_API
#include "lib.h"   // LogSinkFn
ECS_API void EcsInstallLogSink(LogSinkFn fn);
