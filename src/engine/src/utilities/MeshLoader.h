#pragma once

#include <vector>
#include <string>
#include <cstdint>

#include "ApplicationContext.h"

// Utility for loading mesh data from 3D model files using Assimp
namespace MeshLoader
{
    struct MeshMaterial {
        uint32_t Width{0};
        uint32_t Height{0};
        uint32_t MaterialIndex{0};
        std::vector<uint32_t> TextureData; // RGBA8 pixels (Width*Height entries)
    };
    // Load mesh data from a file (supports .obj, .fbx, .gltf, etc. via Assimp)
    // Returns true on success, false on failure (error message written to outError)
    // If the file contains multiple meshes, they are concatenated into single vertex/index arrays
    bool LoadMeshFromFile(
        const char* filePath,
        std::vector<MeshVertex>& outVertices,
        std::vector<uint32_t>& outIndices,
        std::vector<SubMesh>& subMeshes,
        std::vector<MeshMaterial>& outMaterials,
        std::string& outError
    );
}
