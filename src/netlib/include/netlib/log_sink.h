#pragma once
#include "netlib/netlib_api.h"   // NETLIB_API
#include "lib.h"                 // LogSinkFn
namespace netlib { NETLIB_API void SetLogSink(LogSinkFn fn); }
