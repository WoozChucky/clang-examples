#include "MeshManagerPanel.h"
#include "EditorContext.h"
#include "EditorFileDialog.h"

#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>
#include <nvrhi/nvrhi.h>
#include <glm/glm.hpp>
#include <glm/vec3.hpp>

#include "MeshSystem.h"
#include "MaterialSystem.h"
#include "MeshLoader.h"
#include "MeshPreviewRenderer.h"

void MeshManagerPanel::Draw(const EditorContext& ctx)
{
    ImGuiIO& io = ImGui::GetIO();

    // Mesh Manager Window
    ImGui::Begin("Mesh Manager");

    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Mesh System");
    ImGui::Separator();

    if (ctx.MeshSys) {
        const uint32_t meshCount = ctx.MeshSys->GetMeshCount();
        ImGui::Text("Loaded Meshes: %u", meshCount);

        ImGui::Spacing();

        // Display list of loaded meshes with selection
        if (meshCount > 0) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "Available Meshes:");
            for (uint32_t i = 0; i < meshCount; ++i) {
                char label[64];
                snprintf(label, sizeof(label), "Mesh %u", i);
                const bool isSelected = (selectedMeshId == static_cast<int>(i));
                if (ImGui::Selectable(label, isSelected)) {
                    selectedMeshId = static_cast<int>(i);
                }
            }
            ImGui::Separator();

            // Show preview of selected mesh
            if (selectedMeshId >= 0 && selectedMeshId < static_cast<int>(meshCount)) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Mesh Preview:");
                ImGui::Spacing();

                auto meshBounds = ctx.MeshSys->GetMeshBounds(static_cast<uint32_t>(selectedMeshId));
                auto meshResources = ctx.MeshSys->GetMeshResources(static_cast<uint32_t>(selectedMeshId));

                if (meshBounds.valid && meshResources.valid && ctx.Preview) {
                    // 3D mesh preview with camera controls
                    constexpr float previewSize = 256.0f;

                    // Calculate default camera distance based on mesh size
                    const glm::vec3 meshSize = meshBounds.max - meshBounds.min;
                    const float maxExtent = glm::max(glm::max(meshSize.x, meshSize.y), meshSize.z);
                    const float defaultDistance = maxExtent * 2.0f;

                    // Reset camera to default on first view of this mesh
                    if (lastViewedMesh != selectedMeshId)
                    {
                        lastViewedMesh = selectedMeshId;
                        m_Preview.cameraDistance = defaultDistance;
                        m_Preview.cameraYaw = 0.0f;
                        m_Preview.cameraPitch = 0.3f;
                    }

                    // Render mesh to offscreen texture
                    nvrhi::ITexture* previewTexture = ctx.Preview->RenderMeshPreview(
                        ctx.MeshSys,
                        static_cast<uint32_t>(selectedMeshId),
                        m_Preview.cameraDistance,
                        m_Preview.cameraYaw,
                        m_Preview.cameraPitch
                    );

                    if (previewTexture)
                    {
                        const ImVec2 previewPos = ImGui::GetCursorScreenPos();

                        // Display rendered preview
                        ImGui::Image(
                            reinterpret_cast<ImTextureID>(previewTexture),
                            ImVec2(previewSize, previewSize),
                            ImVec2(0, 0), ImVec2(1, 1),
                            ImVec4(1, 1, 1, 1),
                            ImVec4(0.3f, 0.3f, 0.3f, 1.0f) // Border
                        );

                        // Camera controls (drag to rotate, wheel to zoom)
                        if (ImGui::IsItemHovered())
                        {
                            // Mouse wheel zoom
                            if (io.MouseWheel != 0.0f)
                            {
                                m_Preview.cameraDistance *= (1.0f - io.MouseWheel * 0.1f);
                                m_Preview.cameraDistance = glm::clamp(
                                    m_Preview.cameraDistance,
                                    maxExtent * 0.5f,
                                    maxExtent * 10.0f
                                );
                            }

                            // Mouse drag to rotate
                            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                            {
                                if (!m_Preview.isDragging)
                                {
                                    m_Preview.isDragging = true;
                                    m_Preview.lastMouseX = io.MousePos.x;
                                    m_Preview.lastMouseY = io.MousePos.y;
                                }
                                else
                                {
                                    const float deltaX = io.MousePos.x - m_Preview.lastMouseX;
                                    const float deltaY = io.MousePos.y - m_Preview.lastMouseY;

                                    m_Preview.cameraYaw += deltaX * 0.01f;
                                    m_Preview.cameraPitch -= deltaY * 0.01f;

                                    // Clamp pitch to avoid gimbal lock
                                    m_Preview.cameraPitch = glm::clamp(
                                        m_Preview.cameraPitch,
                                        -1.5f, 1.5f
                                    );

                                    m_Preview.lastMouseX = io.MousePos.x;
                                    m_Preview.lastMouseY = io.MousePos.y;
                                }
                            }
                            else
                            {
                                m_Preview.isDragging = false;
                            }
                        }
                        else
                        {
                            m_Preview.isDragging = false;
                        }

                        // Display controls hint
                        ImGui::TextDisabled("(Drag to rotate, scroll to zoom)");
                    }
                    else
                    {
                        ImGui::TextDisabled("(Preview rendering failed)");
                    }

                    ImGui::Spacing();

                    // Display mesh statistics below visualization
                    ImGui::Text("Mesh ID: %d", selectedMeshId);
                    ImGui::Text("Vertices: %u", meshResources.vertexCount);
                    ImGui::Text("Indices: %u", meshResources.indexCount);
                    ImGui::Text("Triangles: %u", meshResources.indexCount / 3);
                    ImGui::Text("Bounds: (%.2f, %.2f, %.2f) - (%.2f, %.2f, %.2f)",
                               meshBounds.min.x, meshBounds.min.y, meshBounds.min.z,
                               meshBounds.max.x, meshBounds.max.y, meshBounds.max.z);
                } else {
                    ImGui::TextDisabled("(No mesh data available)");
                }
            } else {
                ImGui::TextDisabled("Select a mesh to see preview");
            }

            ImGui::Separator();
        }

        // Load mesh from file button
        if (ImGui::Button("Load Mesh from File", ImVec2(200, 0))) {
            char filePath[512] = "";
            const char* filter = "3D Model Files\0*.obj;*.fbx;*.gltf;*.glb;*.dae\0"
                                 "OBJ Files (*.obj)\0*.obj\0"
                                 "FBX Files (*.fbx)\0*.fbx\0"
                                 "GLTF Files (*.gltf;*.glb)\0*.gltf;*.glb\0"
                                 "All Files (*.*)\0*.*\0\0";

            if (EditorFileDialog::Open(filePath, sizeof(filePath), filter)) {
                // File selected, load it using MeshLoader
                std::vector<MeshVertex> vertices;
                std::vector<uint32_t> indices;
                std::vector<MeshLoader::MeshMaterial> materials;
                std::vector<SubMesh> subMeshes;
                std::string error;

                if (MeshLoader::LoadMeshFromFile(filePath, vertices, indices, subMeshes, materials, error)) {
                    bool hasSubMeshes = !subMeshes.empty();
                    // Successfully loaded, upload to GPU via MeshSystem
                    MeshHandle meshHandle = ctx.MeshSys->AddMesh(
                        vertices.data(), static_cast<uint32_t>(vertices.size()),
                        indices.data(), static_cast<uint32_t>(indices.size()),
                        hasSubMeshes ? subMeshes.data() : nullptr,
                        hasSubMeshes ? static_cast<uint32_t>(subMeshes.size()) : 0
                    );

                    for (const auto& mat : materials) {
                        MaterialHandle materialHandle = ctx.MatSys->AddMaterial(
                            mat.TextureData.data(), mat.Width, mat.Height
                        );
                        ctx.MeshSys->AssociateMeshMaterial(meshHandle, materialHandle, mat.MaterialIndex);
                    }

                    if (meshHandle.Index != UINT64_MAX) {
                        snprintf(statusMessage, sizeof(statusMessage),
                                "Success! Loaded mesh %llu (%zu vertices, %zu indices)",
                                (unsigned long long)meshHandle.Index, vertices.size(), indices.size());
                        statusColor = ImVec4(0.4f, 1.0f, 0.4f, 1.0f); // Green
                    } else {
                        snprintf(statusMessage, sizeof(statusMessage),
                                "Failed to upload mesh to GPU");
                        statusColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); // Red
                    }
                } else {
                    // Failed to load
                    snprintf(statusMessage, sizeof(statusMessage),
                            "Failed to load mesh: %s", error.c_str());
                    statusColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); // Red
                }
            }
        }

        // Display status message if any
        if (statusMessage[0] != '\0') {
            ImGui::Spacing();
            ImGui::TextColored(statusColor, "%s", statusMessage);
        }

    } else {
        ImGui::TextDisabled("MeshSystem not available");
    }

    ImGui::End();
}
