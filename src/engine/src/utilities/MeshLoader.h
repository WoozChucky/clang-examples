#pragma once

#include <vector>
#include <string>
#include <cstdint>

#include "Engine.h"
#include "ApplicationContext.h"
#include "Skeleton.h"
#include "AnimationClip.h"
#include "Skinning.h"

// Utility for loading mesh data from 3D model files using Assimp
namespace MeshLoader
{
    struct MeshMaterial {
        uint32_t Width{0};
        uint32_t Height{0};
        uint32_t MaterialIndex{0};
        std::vector<uint32_t> TextureData; // RGBA8 pixels (Width*Height entries)
    };

    // Fully-decoded CPU model: geometry + skeleton + per-vertex skinning + animation clips + decoded
    // material pixels. A single assimp scene-walk fills all of it (see LoadModel). Indices are GLOBAL
    // (baseVertex + face index per submesh) so the whole model shares one vertex/index buffer.
    struct LoadedModel {
        std::vector<MeshVertex>    vertices;
        std::vector<uint32_t>      indices;     // GLOBAL: baseVertex + face index, per submesh
        std::vector<SubMesh>       subMeshes;
        std::vector<MeshMaterial>  materials;
        bool                       hasSkeleton = false;
        Skeleton                   skeleton;
        std::vector<SkinnedVertex> skinning;     // aligned 1:1 to vertices; empty if !hasSkeleton
        std::vector<AnimationClip> clips;
    };
    ENGINE_API bool LoadModel(const char* filePath, LoadedModel& out, std::string& outError);
    // Load mesh data from a file (supports .obj, .fbx, .gltf, etc. via Assimp)
    // Returns true on success, false on failure (error message written to outError)
    // If the file contains multiple meshes, they are concatenated into single vertex/index arrays
    ENGINE_API bool LoadMeshFromFile(
        const char* filePath,
        std::vector<MeshVertex>& outVertices,
        std::vector<uint32_t>& outIndices,
        std::vector<SubMesh>& subMeshes,
        std::vector<MeshMaterial>& outMaterials,
        std::string& outError
    );
}
