#pragma once
#include <cstdint>
#include "GameStateId.h"

// True if an entity scoped by `mask` is active in state `cur`. mask == 0 means "always-on"
// (no StateScopeComponent / unscoped). Bit i set => active in GameStateId value i.
inline bool ScopeAllows(uint32_t mask, GameStateId cur) {
    return mask == 0u || (mask & (1u << static_cast<uint32_t>(cur))) != 0u;
}
