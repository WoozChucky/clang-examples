#pragma once

#include "lib.h"
#include "glm/vec3.hpp"

struct VertexPacked {
    glm::vec3 pos;          // 12 bytes
    uint32_t materialFlags; // 4 bytes - lower 16 bits = material/tile index, top bits = flags
    uint32_t tilePacked;    // 2 bytes - tileX (lo8) | tileY (hi8)
    uint32_t packedLightUV;   // 2 bytes - blockLight (bits 0..3), skyLight (4..7), AO (8..11)
};

static_assert(sizeof(VertexPacked) == 24, "VertexPacked must be 24 bytes to match input layout");

inline uint16_t PackTile(int tileX, int tileY) {
    SM_ASSERT((tileX & ~0xFF) == 0 && (tileY & ~0xFF) == 0, "Tile coordinates out of bounds for packing");
    return static_cast<uint16_t>((tileX & 0xFF) | ((tileY & 0xFF) << 8));
}

inline uint16_t PackLight(int blockLight, int skyLight, int ao)
{
    // clamp 0..15
    blockLight = blockLight < 0 ? 0 : (blockLight > 15 ? 15 : blockLight);
    skyLight   = skyLight < 0   ? 0 : (skyLight > 15   ? 15 : skyLight);
    ao         = ao < 0         ? 0 : (ao > 15         ? 15 : ao);
    return static_cast<uint16_t>((blockLight & 0xF) | ((skyLight & 0xF) << 4) | ((ao & 0xF) << 8));
}

inline uint16_t PackUVFace(int uBit, int vBit, int faceIndex)
{
    SM_ASSERT((uBit == 0 || uBit == 1) && (vBit == 0 || vBit == 1), "uBit and vBit must be 0 or 1");
    SM_ASSERT(faceIndex >= 0 && faceIndex < 256, "faceIndex out of bounds for packing");
    return static_cast<uint16_t>((faceIndex << 8) | ((vBit & 1) << 1) | (uBit & 1));
}