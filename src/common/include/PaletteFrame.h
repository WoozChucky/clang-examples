#pragma once
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>
#include "ECS.h"   // EntityId

// Per-tick bone-matrix palettes for all skinned entities, published GameThread->RenderThread via an
// atomic shared_ptr (parallel to LatestWorldSnapshot; immutable once published). Flat matrix array +
// per-entity ranges = one StructuredBuffer + a per-instance base offset on the GPU.
struct PaletteFrame {
    std::vector<glm::mat4> matrices;
    struct Range { EntityId entity; uint32_t offset; uint32_t count; };
    std::vector<Range> ranges;
};
