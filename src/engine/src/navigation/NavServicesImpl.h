#pragma once

#include "NavServices.h"

namespace NavServicesImpl {
    // Populate the function pointer table. Safe to call multiple times
    // (idempotent — writes the same pointers). GameThread or RenderThread
    // can call; subsequent calls are no-ops since pointers are constant.
    void Init(NavServices& out);
}
