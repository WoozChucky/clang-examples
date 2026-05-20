#pragma once
#include <cstdint>
#include <Engine.h>

namespace Engine {

enum class MemCategory : uint8_t {
    General, FrameTransient, Renderer, Mesh, Material,
    Texture, UI, ECS, Snapshot, Game, Count
};

// Human-readable name for the memory panel. Exported from Engine.dll.
ENGINE_API const char* ToString(MemCategory c);

} // namespace Engine
