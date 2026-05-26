#pragma once
#include <cstdint>

// Game lifecycle state. Lives in common (not game.h) so ECS components and engine code
// can reference it; game.h gets it transitively via ECS.h.
enum class GameStateId : uint32_t {
    Uninitialized = 0,
    MainMenu      = 1,
    InLevel       = 2,
    InEditor      = 3,
    Paused        = 4,
};
