#pragma once

#include "Engine.h"
#include "NetServices.h"

namespace NetServicesImpl {
    ENGINE_API void Init(NetServices& out);   // forwards the table to NetSubsystem::Instance()
}
