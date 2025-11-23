#pragma once

#include "ECS.h"

namespace WorldManager {
    const auto DEFAULT_WORLD_SNAPSHOT_PATH = "world.json";
    bool SaveWorldSnapshot(const std::string& filepath, const ECS* world);
    bool LoadWorldSnapshot(const std::string& filepath, ECS* world);
}
