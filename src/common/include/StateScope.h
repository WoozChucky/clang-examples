#pragma once
#include <cstdint>

// True if an entity scoped by `mask` is active in state index `cur`. mask == 0 means
// "always-on" (no StateScopeComponent / unscoped). Bit i set => active in state index i.
// `cur` is an opaque game-owned state index (see the game's GameStates.h); the engine
// does not name the states.
inline bool ScopeAllows(uint32_t mask, uint32_t cur) {
    return mask == 0u || (mask & (1u << cur)) != 0u;
}
