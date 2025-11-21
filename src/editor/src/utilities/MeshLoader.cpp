#include "MeshLoader.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "lib.h"

namespace MeshLoader
{
    // Helper to process a single aiMesh into vertices and indices
    static void ProcessMesh(
        aiMesh* mesh,
        const aiScene* scene,
        std::vector<MeshVertex>& outVertices,
        std::vector<uint32_t>& outIndices)
    {
        const uint32_t baseVertex = static_cast<uint32_t>(outVertices.size());

        // Extract vertices
        for (size_t i = 0; i < mesh->mNumVertices; ++i)
        {
            MeshVertex vertex{};

            // Position
            vertex.px = mesh->mVertices[i].x;
            vertex.py = mesh->mVertices[i].y;
            vertex.pz = mesh->mVertices[i].z;

            // Normal
            if (mesh->HasNormals())
            {
                vertex.nx = mesh->mNormals[i].x;
                vertex.ny = mesh->mNormals[i].y;
                vertex.nz = mesh->mNormals[i].z;
            }
            else
            {
                vertex.nx = 0.0f;
                vertex.ny = 0.0f;
                vertex.nz = 0.0f;
            }

            // Texture coordinates (first UV channel)
            if (mesh->HasTextureCoords(0))
            {
                vertex.u = mesh->mTextureCoords[0][i].x;
                vertex.v = mesh->mTextureCoords[0][i].y;
            }
            else
            {
                vertex.u = 0.0f;
                vertex.v = 0.0f;
            }

            outVertices.push_back(vertex);
        }

        // Extract indices
        for (size_t i = 0; i < mesh->mNumFaces; ++i)
        {
            const aiFace& face = mesh->mFaces[i];
            for (size_t j = 0; j < face.mNumIndices; ++j)
            {
                outIndices.push_back(baseVertex + face.mIndices[j]);
            }
        }

        // Log material info (for future material loading support)
        if (mesh->mMaterialIndex >= 0)
        {
            aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
            if (material)
            {
                SM_TRACE("MeshLoader: Mesh '%s' has material (future: load material here)", mesh->mName.C_Str());
            }
        }
    }

    // Recursive helper to process scene node hierarchy
    static void ProcessNode(
        aiNode* node,
        const aiScene* scene,
        std::vector<MeshVertex>& outVertices,
        std::vector<uint32_t>& outIndices)
    {
        // Process all meshes in this node
        for (size_t i = 0; i < node->mNumMeshes; ++i)
        {
            aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
            ProcessMesh(mesh, scene, outVertices, outIndices);
        }

        // Recursively process child nodes
        for (size_t i = 0; i < node->mNumChildren; ++i)
        {
            ProcessNode(node->mChildren[i], scene, outVertices, outIndices);
        }
    }

    bool LoadMeshFromFile(
        const char* filePath,
        std::vector<MeshVertex>& outVertices,
        std::vector<uint32_t>& outIndices,
        std::string& outError)
    {
        if (!filePath)
        {
            outError = "Invalid file path (null)";
            return false;
        }

        // Clear output arrays
        outVertices.clear();
        outIndices.clear();

        // Create Assimp importer
        Assimp::Importer importer;

        // Load the file with post-processing flags
        const aiScene* scene = importer.ReadFile(filePath,
            aiProcess_Triangulate |           // Convert all primitives to triangles
            aiProcess_GenSmoothNormals |      // Generate smooth normals if missing
            aiProcess_FlipUVs |               // Flip V coordinate (OpenGL convention)
            aiProcess_JoinIdenticalVertices   // Optimize by joining identical vertices
        );

        // Check for errors
        if (!scene || !scene->mRootNode || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE))
        {
            outError = std::string("Assimp failed to load file: ") + importer.GetErrorString();
            SM_ERROR("MeshLoader: %s", outError.c_str());
            return false;
        }

        // Process the scene hierarchy
        ProcessNode(scene->mRootNode, scene, outVertices, outIndices);

        // Validate results
        if (outVertices.empty() || outIndices.empty())
        {
            outError = "No mesh data found in file (no vertices or indices)";
            SM_WARN("MeshLoader: %s", outError.c_str());
            return false;
        }

        SM_TRACE("MeshLoader: Successfully loaded '%s' - %zu vertices, %zu indices",
                 filePath, outVertices.size(), outIndices.size());

        return true;
    }
}
