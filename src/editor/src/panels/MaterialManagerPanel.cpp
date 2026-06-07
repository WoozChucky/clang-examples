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
#include "AssetKey.h"

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

        // Display list of loaded materials with selection
        if (materialCount > 0) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "Available Materials:");
            for (const auto& [handle, key] : ctx.MatSys->GetAssetList()) {
                const bool isSelected = (hasSelection && handle == selectedMaterialId);
                const char* label = key.empty() ? "(default)" : key.c_str();
                if (ImGui::Selectable(label, isSelected)) {
                    selectedMaterialId = handle;
                    hasSelection = true;
                }
            }
            ImGui::Separator();

            // Show preview of selected material
            if (hasSelection) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Material Preview:");
                ImGui::Spacing();

                auto materialResources = ctx.MatSys->GetMaterialResources(selectedMaterialId);
                if (materialResources.valid && materialResources.texture) {
                    // Cast nvrhi texture to ImTextureID for ImGui::Image()
                    ImTextureID texId = (ImTextureID)(nvrhi::ITexture*)materialResources.texture.Get();

                    // Display texture preview (256x256 size)
                    constexpr float previewSize = 256.0f;
                    ImGui::Image(texId, ImVec2(previewSize, previewSize));

                    ImGui::Text("Material ID: %llu", (unsigned long long)selectedMaterialId);
                } else {
                    ImGui::TextDisabled("(No texture available for this material)");
                }
            } else {
                ImGui::TextDisabled("Select a material to see preview");
            }

            ImGui::Separator();
        }

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
                        NormalizeAssetKey(filePath), pixels.data(), width, height
                    );

                    if (handle.Index != UINT64_MAX) {
                        snprintf(statusMessage, sizeof(statusMessage),
                                "Success! Loaded material %llu (%ux%u pixels)",
                                (unsigned long long)handle.Index, width, height);
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
