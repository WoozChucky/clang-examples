#pragma once

#include <cstdint>

#include "ECS.h"   // NavMeshConfigComponent

// Number of class meshes to build: ClassCount clamped to at least 1.
inline uint8_t NavLiveClassCount(const NavMeshConfigComponent& cfg) {
    return cfg.ClassCount > 0 ? cfg.ClassCount : uint8_t{1};
}

// ResolveNavClass is added in Task 2 (needs NavClassComponent).
