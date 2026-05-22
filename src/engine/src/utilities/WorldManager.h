#pragma once

#include "Engine.h"
#include "ECS.h"

namespace WorldManager {
    const auto DEFAULT_WORLD_SNAPSHOT_PATH = "world.json";
    ENGINE_API bool SaveWorldSnapshot(const std::string& filepath, const ECS* world);
    ENGINE_API bool LoadWorldSnapshot(const std::string& filepath, ECS* world);
}
