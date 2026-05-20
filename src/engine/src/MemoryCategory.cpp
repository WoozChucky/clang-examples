#include <memory/MemoryCategory.h>

namespace Engine {

const char* ToString(MemCategory c) {
    switch (c) {
        case MemCategory::General:        return "General";
        case MemCategory::FrameTransient: return "FrameTransient";
        case MemCategory::Renderer:       return "Renderer";
        case MemCategory::Mesh:           return "Mesh";
        case MemCategory::Material:       return "Material";
        case MemCategory::Texture:        return "Texture";
        case MemCategory::UI:             return "UI";
        case MemCategory::ECS:            return "ECS";
        case MemCategory::Snapshot:       return "Snapshot";
        case MemCategory::Game:           return "Game";
        default:                          return "?";
    }
}

} // namespace Engine
