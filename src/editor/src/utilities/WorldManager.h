#pragma once

#include "ECS.h"

namespace WorldManager {

    bool SaveWorldSnapshot(const std::string& filepath, const ECS& world);

}
