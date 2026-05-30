#pragma once

#include "netlib/netlib_api.h"

namespace netlib {

// Returns a static version string. Smoke-test hook to prove the DLL links + loads.
NETLIB_API const char* Version();

} // namespace netlib
