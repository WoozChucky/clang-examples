#include "MaterialManagerPanel.h"
#include "EditorContext.h"
#include "EditorFileDialog.h"

#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>
#include <nvrhi/nvrhi.h>

#include "MaterialSystem.h"
#include "MaterialLoader.h"
#include "ApplicationContext.h"

void MaterialManagerPanel::Draw(const EditorContext& ctx)
{
    // Material Manager Window
    ImGui::Begin("Material Manager");

    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Material System");
    ImGui::Separator();

    if (ctx.MatSys) {
        const uint32_t materialCount = ctx.MatSys->GetMaterialCount();
        ImGui::Text("Loaded Materials: %u", materialCount);

        ImGui::Spacing();

        // Track selected material

        // Display list of loaded materials with selection
        if (materialCount > 0) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "Available Materials:");
            for (uint32_t i = 0; i < materialCount; ++i) {
                char label[64];
                snprintf(label, sizeof(label), "Material %u", i);
                const bool isSelected = (selectedMaterialId == static_cast<int>(i));
                if (ImGui::Selectable(label, isSelected)) {
                    selectedMaterialId = static_cast<int>(i);
                }
            }
            ImGui::Separator();

            // Show preview of selected material
            if (selectedMaterialId >= 0 && selectedMaterialId < static_cast<int>(materialCount)) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Material Preview:");
                ImGui::Spacing();

                auto materialResources = ctx.MatSys->GetMaterialResources(static_cast<uint32_t>(selectedMaterialId));
                if (materialResources.valid && materialResources.texture) {
                    // Cast nvrhi texture to ImTextureID for ImGui::Image()
                    ImTextureID texId = (ImTextureID)(nvrhi::ITexture*)materialResources.texture.Get();

                    // Display texture preview (256x256 size)
                    constexpr float previewSize = 256.0f;
                    ImGui::Image(texId, ImVec2(previewSize, previewSize));

                    ImGui::Text("Material ID: %d", selectedMaterialId);
                } else {
                    ImGui::TextDisabled("(No texture available for this material)");
                }
            } else {
                ImGui::TextDisabled("Select a material to see preview");
            }

            ImGui::Separator();
        }

        // Load material from file button

        if (ImGui::Button("Load Material from File", ImVec2(200, 0))) {
            char filePath[512] = "";
            const char* filter = "Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.hdr\0"
                                 "PNG Files (*.png)\0*.png\0"
                                 "JPEG Files (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0"
                                 "BMP Files (*.bmp)\0*.bmp\0"
                                 "TGA Files (*.tga)\0*.tga\0"
                                 "HDR Files (*.hdr)\0*.hdr\0"
                                 "All Files (*.*)\0*.*\0\0";

            if (EditorFileDialog::Open(filePath, sizeof(filePath), filter)) {
                // File selected, load it using MaterialLoader
                std::vector<uint32_t> pixels;
                uint32_t width = 0;
                uint32_t height = 0;
                std::string error;

                if (MaterialLoader::LoadMaterialFromFile(filePath, pixels, width, height, error)) {
                    // Successfully loaded, upload to GPU via MaterialSystem
                    MaterialHandle handle = ctx.MatSys->AddMaterial(
                        pixels.data(), width, height
                    );

                    if (handle.Index != UINT32_MAX) {
                        snprintf(statusMessage, sizeof(statusMessage),
                                "Success! Loaded material %u (%ux%u pixels)",
                                handle.Index, width, height);
                        statusColor = ImVec4(0.4f, 1.0f, 0.4f, 1.0f); // Green
                    } else {
                        snprintf(statusMessage, sizeof(statusMessage),
                                "Failed to upload material to GPU");
                        statusColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); // Red
                    }
                } else {
                    // Failed to load
                    snprintf(statusMessage, sizeof(statusMessage),
                            "Failed to load image: %s", error.c_str());
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
        ImGui::TextDisabled("MaterialSystem not available");
    }

    ImGui::End();
}
