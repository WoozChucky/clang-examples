#include "MeshLoader.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "lib.h"
#include "MaterialLoader.h"

namespace MeshLoader
{
    // Helper to process a single aiMesh into vertices and indices
    static void ProcessMesh(
        aiMesh* mesh,
        const aiScene* scene,
        std::vector<MeshVertex>& outVertices,
        std::vector<uint32_t>& outIndices,
        std::vector<SubMesh>& outSubMeshes,
        std::vector<MeshMaterial>& outMaterials)
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

        SubMesh subMesh{};
        subMesh.IndexStart = static_cast<uint32_t>(outIndices.size());

        // Extract indices
        for (size_t i = 0; i < mesh->mNumFaces; ++i)
        {
            const aiFace& face = mesh->mFaces[i];
            for (size_t j = 0; j < face.mNumIndices; ++j)
            {
                outIndices.push_back(baseVertex + face.mIndices[j]);
            }
        }

        subMesh.IndexCount = static_cast<uint32_t>(outIndices.size()) - subMesh.IndexStart;

        // Log material info (for future material loading support)
        if (mesh->mMaterialIndex) {
            aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
            if (material) {
                aiString texPath;
                for (size_t i = 0; i < material->mNumProperties; ++i) {
                    auto prop = material->mProperties[i];
                    SM_TRACE("Material property: key='%s', semantic=%d, index=%d type=%d length=%u",
                             prop->mKey.C_Str(), prop->mSemantic, prop->mIndex,
                             static_cast<int>(prop->mType), prop->mDataLength);
                }
                // The material might be a texture image, or a color
                auto color = aiColor4D{};
                if (AI_SUCCESS == material->Get(AI_MATKEY_COLOR_DIFFUSE, color)) {
                    SM_TRACE("Material diffuse color: r=%.3f g=%.3f b=%.3f a=%.3f",
                             color.r, color.g, color.b, color.a);
                }

                if (material->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) {
                    std::string fullTexPath = std::string(texPath.C_Str());

                    std::vector<uint32_t> pixels;
                    uint32_t width = 0;
                    uint32_t height = 0;
                    std::string error;
                    if (!MaterialLoader::LoadMaterialFromFile(fullTexPath.c_str(), pixels, width, height, error)) {
                        SM_WARN("Failed to load material '%s': %s", fullTexPath.c_str(), error.c_str());
                    } else {
                        MeshMaterial meshMat{};
                        meshMat.Width = width;
                        meshMat.Height = height;
                        meshMat.MaterialIndex = mesh->mMaterialIndex;
                        subMesh.MaterialIndex = meshMat.MaterialIndex;
                        meshMat.TextureData = std::move(pixels);
                        outMaterials.push_back(std::move(meshMat));
                        SM_TRACE("Loaded material from '%s' (%ux%u)", fullTexPath.c_str(), width, height);
                    }
                }
            }
        }

        outSubMeshes.push_back(subMesh);
    }

    // Recursive helper to process scene node hierarchy
    static void ProcessNode(
        aiNode* node,
        const aiScene* scene,
        std::vector<MeshVertex>& outVertices,
        std::vector<uint32_t>& outIndices,
        std::vector<SubMesh>& outSubMeshes,
        std::vector<MeshMaterial>& outMaterials)
    {
        // Process all meshes in this node
        for (size_t m = 0; m < node->mNumMeshes; ++m) {
            aiMesh* mesh = scene->mMeshes[node->mMeshes[m]];
            ProcessMesh(mesh, scene, outVertices, outIndices, outSubMeshes, outMaterials);
        }

        // Recursively process child nodes
        for (size_t i = 0; i < node->mNumChildren; i++) {
            ProcessNode(node->mChildren[i], scene, outVertices, outIndices, outSubMeshes, outMaterials);
        }
    }

    bool LoadMeshFromFile(
        const char* filePath,
        std::vector<MeshVertex>& outVertices,
        std::vector<uint32_t>& outIndices,
        std::vector<SubMesh>& outSubMeshes,
        std::vector<MeshMaterial>& outMaterials,
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
        ProcessNode(scene->mRootNode, scene, outVertices, outIndices, outSubMeshes, outMaterials);

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
