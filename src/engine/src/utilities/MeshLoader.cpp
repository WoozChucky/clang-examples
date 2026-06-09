#include "MeshLoader.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <unordered_map>
#include <filesystem>

#include "lib.h"
#include "MaterialLoader.h"
#include "Skeleton.h"
#include "AnimationClip.h"
#include "Skinning.h"

namespace {
constexpr unsigned kAssimpFlags =
    aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_FlipUVs | aiProcess_JoinIdenticalVertices;
glm::mat4 AiToGlm(const aiMatrix4x4& m) {
    return glm::mat4(m.a1,m.b1,m.c1,m.d1, m.a2,m.b2,m.c2,m.d2,
                     m.a3,m.b3,m.c3,m.d3, m.a4,m.b4,m.c4,m.d4);
}
}

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

    // Unified loader: a single assimp scene-walk filling geometry (GLOBAL indices), skeleton,
    // per-vertex skinning, animation clips, and decoded material pixels. Port of the inline loader
    // in GameThread.cpp combined with the static loader's global-index rule.
    bool LoadModel(const char* filePath, LoadedModel& out, std::string& outError)
    {
        if (!filePath) { outError = "Invalid file path (null)"; return false; }

        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(filePath, kAssimpFlags);
        if (!scene) { outError = importer.GetErrorString(); return false; }

        const std::filesystem::path modelDir = std::filesystem::path(filePath).parent_path();

        // --- Geometry walk: GLOBAL indices (baseVertex + face index per submesh) ---
        auto processMesh = [&](const aiMesh* mesh) {
            const uint32_t baseVertex = static_cast<uint32_t>(out.vertices.size());

            for (size_t i = 0; i < mesh->mNumVertices; ++i) {
                MeshVertex vertex{};
                vertex.px = mesh->mVertices[i].x;
                vertex.py = mesh->mVertices[i].y;
                vertex.pz = mesh->mVertices[i].z;
                if (mesh->HasNormals()) {
                    vertex.nx = mesh->mNormals[i].x;
                    vertex.ny = mesh->mNormals[i].y;
                    vertex.nz = mesh->mNormals[i].z;
                } else {
                    vertex.nx = 0.0f; vertex.ny = 0.0f; vertex.nz = 0.0f;
                }
                if (mesh->HasTextureCoords(0)) {
                    vertex.u = mesh->mTextureCoords[0][i].x;
                    vertex.v = mesh->mTextureCoords[0][i].y;
                } else {
                    vertex.u = 0.0f; vertex.v = 0.0f;
                }
                out.vertices.push_back(vertex);
            }

            SubMesh subMesh{};
            subMesh.IndexStart = static_cast<uint32_t>(out.indices.size());
            for (size_t i = 0; i < mesh->mNumFaces; ++i) {
                const aiFace& face = mesh->mFaces[i];
                for (size_t j = 0; j < face.mNumIndices; ++j)
                    out.indices.push_back(baseVertex + face.mIndices[j]);
            }
            subMesh.IndexCount = static_cast<uint32_t>(out.indices.size()) - subMesh.IndexStart;

            if (mesh->mMaterialIndex < UINT32_MAX) {
                aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
                if (material) {
                    aiString texPath;
                    if (material->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) {
                        std::string fullTexPath = std::string(texPath.C_Str());
                        {
                            std::filesystem::path tp(fullTexPath);
                            if (tp.is_relative() && !modelDir.empty())
                                fullTexPath = (modelDir / tp).string();
                        }
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
                            out.materials.push_back(std::move(meshMat));
                            SM_TRACE("Loaded material from '%s' (%ux%u)", fullTexPath.c_str(), width, height);
                        }
                    }
                }
            }

            out.subMeshes.push_back(subMesh);
        };

        std::function<void(const aiNode*)> processNode = [&](const aiNode* node) {
            for (size_t m = 0; m < node->mNumMeshes; ++m)
                processMesh(scene->mMeshes[node->mMeshes[m]]);
            for (size_t i = 0; i < node->mNumChildren; ++i)
                processNode(node->mChildren[i]);
        };
        processNode(scene->mRootNode);

        // --- Skeleton extraction ---
        {
            // 1) Collect every bone (by name) referenced across the scene's meshes -> inverse-bind.
            std::unordered_map<std::string, glm::mat4> boneInverseBind;
            for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
                const aiMesh* mesh = scene->mMeshes[mi];
                for (unsigned bi = 0; bi < mesh->mNumBones; ++bi) {
                    const aiBone* bone = mesh->mBones[bi];
                    boneInverseBind[bone->mName.C_Str()] = AiToGlm(bone->mOffsetMatrix);
                }
            }
            // 2) Walk the node tree (pre-order => parent emitted before child = topological order).
            //    A node whose name matches a bone becomes a Skeleton bone; parent = nearest ancestor bone.
            if (!boneInverseBind.empty()) {
                Skeleton skel;
                std::unordered_map<std::string,int> boneNameToIndex;
                // `parentGlobal` accumulates the transforms of NON-bone ancestor nodes (e.g. a "Z_UP"
                // up-axis-correction matrix or an "Armature" node) above the skeleton root. The ROOT
                // bone folds that chain into its localBind so the correction isn't lost (CesiumMan's
                // skeleton sits under Z_UP/Armature non-bone nodes). Non-root bones inherit it via
                // their parent bone, so they use only their own node transform.
                std::function<void(const aiNode*, int, const glm::mat4&)> walk =
                    [&](const aiNode* node, int parentBoneIdx, const glm::mat4& parentGlobal) {
                    const glm::mat4 nodeLocal = AiToGlm(node->mTransformation);
                    int myIdx = parentBoneIdx;
                    glm::mat4 childParentGlobal = parentGlobal * nodeLocal; // accumulate until the first bone
                    auto it = boneInverseBind.find(node->mName.C_Str());
                    if (it != boneInverseBind.end()) {
                        Bone bone;
                        bone.name        = node->mName.C_Str();
                        bone.parent      = parentBoneIdx;
                        bone.localBind   = (parentBoneIdx < 0) ? (parentGlobal * nodeLocal) : nodeLocal;
                        bone.inverseBind = it->second;
                        // The non-bone ancestor chain above the FIRST root bone (e.g. Z_UP * Armature)
                        // is the authored->engine correction; captured here, prepended to palettes.
                        if (parentBoneIdx < 0) skel.rootTransform = parentGlobal;
                        myIdx = static_cast<int>(skel.bones.size());
                        boneNameToIndex[bone.name] = myIdx;
                        skel.bones.push_back(std::move(bone));
                        childParentGlobal = glm::mat4(1.0f); // descendants are under a bone (parent index carries the chain)
                    }
                    for (unsigned c = 0; c < node->mNumChildren; ++c)
                        walk(node->mChildren[c], myIdx, childParentGlobal);
                };
                walk(scene->mRootNode, -1, glm::mat4(1.0f));
                if (!skel.bones.empty()) {
                    out.skeleton    = std::move(skel);
                    out.hasSkeleton = true;
                    SM_TRACE("Skeleton extracted: %zu bones", out.skeleton.bones.size());

                    // Per-vertex weights, aligned to out.vertices. Accumulate influences in the SAME
                    // mesh concatenation order processNode used, then reduce to top-4. Bone index = the
                    // Skeleton's index (by name).
                    std::vector<std::vector<std::pair<uint32_t,float>>> perVertex(out.vertices.size());
                    uint32_t base = 0;
                    std::function<void(const aiNode*)> collect = [&](const aiNode* node) {
                        for (unsigned m = 0; m < node->mNumMeshes; ++m) {
                            const aiMesh* mesh = scene->mMeshes[node->mMeshes[m]];
                            for (unsigned bi = 0; bi < mesh->mNumBones; ++bi) {
                                const aiBone* bone = mesh->mBones[bi];
                                auto ni = boneNameToIndex.find(bone->mName.C_Str());
                                if (ni == boneNameToIndex.end()) continue;
                                const uint32_t boneIdx = static_cast<uint32_t>(ni->second);
                                for (unsigned w = 0; w < bone->mNumWeights; ++w) {
                                    const aiVertexWeight& vw = bone->mWeights[w];
                                    const size_t vtx = static_cast<size_t>(base) + vw.mVertexId;
                                    if (vtx < perVertex.size() && vw.mWeight > 0.0f)
                                        perVertex[vtx].emplace_back(boneIdx, vw.mWeight);
                                }
                            }
                            base += mesh->mNumVertices;
                        }
                        for (unsigned c = 0; c < node->mNumChildren; ++c) collect(node->mChildren[c]);
                    };
                    collect(scene->mRootNode);

                    out.skinning.resize(out.vertices.size());
                    for (size_t v = 0; v < out.vertices.size(); ++v)
                        out.skinning[v] = MakeSkinnedVertex(std::move(perVertex[v]));
                    SM_TRACE("Skinning extracted: %zu verts", out.skinning.size());

                    // --- Animation clip extraction ---
                    for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
                        const aiAnimation* anim = scene->mAnimations[a];
                        const double tps = (anim->mTicksPerSecond != 0.0) ? anim->mTicksPerSecond : 25.0;
                        AnimationClip clip;
                        clip.name     = anim->mName.length ? anim->mName.C_Str() : ("clip" + std::to_string(a));
                        clip.duration = static_cast<float>(anim->mDuration / tps);
                        for (unsigned c = 0; c < anim->mNumChannels; ++c) {
                            const aiNodeAnim* nodeAnim = anim->mChannels[c];
                            auto ni = boneNameToIndex.find(nodeAnim->mNodeName.C_Str());
                            if (ni == boneNameToIndex.end()) continue; // channel targets a non-bone node
                            AnimChannel ch;
                            ch.boneIndex = ni->second;
                            ch.posKeys.reserve(nodeAnim->mNumPositionKeys);
                            for (unsigned k = 0; k < nodeAnim->mNumPositionKeys; ++k) {
                                const aiVectorKey& vk = nodeAnim->mPositionKeys[k];
                                ch.posKeys.emplace_back(static_cast<float>(vk.mTime / tps), glm::vec3(vk.mValue.x, vk.mValue.y, vk.mValue.z));
                            }
                            ch.rotKeys.reserve(nodeAnim->mNumRotationKeys);
                            for (unsigned k = 0; k < nodeAnim->mNumRotationKeys; ++k) {
                                const aiQuatKey& qk = nodeAnim->mRotationKeys[k];
                                ch.rotKeys.emplace_back(static_cast<float>(qk.mTime / tps), glm::quat(qk.mValue.w, qk.mValue.x, qk.mValue.y, qk.mValue.z));
                            }
                            ch.scaleKeys.reserve(nodeAnim->mNumScalingKeys);
                            for (unsigned k = 0; k < nodeAnim->mNumScalingKeys; ++k) {
                                const aiVectorKey& sk2 = nodeAnim->mScalingKeys[k];
                                ch.scaleKeys.emplace_back(static_cast<float>(sk2.mTime / tps), glm::vec3(sk2.mValue.x, sk2.mValue.y, sk2.mValue.z));
                            }
                            clip.channels.push_back(std::move(ch));
                        }
                        out.clips.push_back(std::move(clip));
                        SM_TRACE("Animation extracted: '%s' (%.2fs, %zu channels)",
                                 out.clips.back().name.c_str(),
                                 out.clips.back().duration, out.clips.back().channels.size());
                    }
                }
            }
        }

        return !out.vertices.empty();
    }
}
