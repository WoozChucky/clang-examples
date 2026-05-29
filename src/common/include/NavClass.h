#pragma once

#include <cstdint>

#include "ECS.h"   // NavMeshConfigComponent

// Number of class meshes to build: ClassCount clamped to at least 1.
inline uint8_t NavLiveClassCount(const NavMeshConfigComponent& cfg) {
    return cfg.ClassCount > 0 ? cfg.ClassCount : uint8_t{1};
}

// Resolve an entity's nav class: its NavClassComponent.ClassId clamped to
// [0, classCount). Missing component or out-of-range → 0.
inline uint8_t ResolveNavClass(const ECS& world, EntityId e, uint8_t classCount) {
    const auto* nc = world.GetComponent<NavClassComponent>(e);
    if (!nc || classCount == 0) return 0;
    return (nc->ClassId < classCount) ? nc->ClassId : uint8_t{0};
}
