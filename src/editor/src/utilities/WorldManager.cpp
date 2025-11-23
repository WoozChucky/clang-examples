#include "WorldManager.h"

#include <fstream>

#include "lib.h"


bool WorldManager::SaveWorldSnapshot(const std::string& filepath, const ECS& world) {

    std::ofstream ofs(filepath, std::ios::binary);
    if (!ofs.is_open()) {
        SM_WARN("");
        return false;
    }


    return true;
}
