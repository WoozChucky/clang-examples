#pragma once

#include <nvrhi/nvrhi.h>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <cstdint>

class MeshSystem;
class Renderer;

// MeshPreviewRenderer: Renders a 3D preview of a mesh to an offscreen texture
// for display in ImGui windows (e.g., Mesh Manager)
class MeshPreviewRenderer
{
public:
    MeshPreviewRenderer() = default;
    ~MeshPreviewRenderer() = default;

    // Initialize with device, renderer, and preview resolution
    bool Initialize(nvrhi::IDevice* device, Renderer* renderer, uint32_t width, uint32_t height);

    // Render a mesh preview to the offscreen texture
    // Returns the rendered texture handle (can be cast to ImTextureID for ImGui::Image)
    nvrhi::ITexture* RenderMeshPreview(
        MeshSystem* meshSystem,
        uint64_t meshId,
        float cameraDistance,
        float cameraYaw,
        float cameraPitch);

    // Update preview resolution (recreates render targets)
    void Resize(uint32_t width, uint32_t height);

    // Cleanup
    void Shutdown();

    // Get current preview texture (for ImGui display)
    nvrhi::ITexture* GetPreviewTexture() const { return m_ColorTexture.Get(); }

private:
    void CreateRenderTargets();
    void CreateShaders();
    void CreatePipeline();

private:
    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;
    uint32_t m_Width = 256;
    uint32_t m_Height = 256;

    // Offscreen render targets
    nvrhi::TextureHandle m_ColorTexture;
    nvrhi::TextureHandle m_DepthTexture;
    nvrhi::FramebufferHandle m_Framebuffer;

    // Rendering resources
    nvrhi::ShaderHandle m_VS;
    nvrhi::ShaderHandle m_PS;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BindingSetHandle m_BindingSet;
    nvrhi::GraphicsPipelineHandle m_Pipeline;

    // Constant buffer for MVP + lighting
    struct PreviewCB
    {
        glm::mat4 MVP;
        glm::mat4 Model;
        glm::vec3 CameraPos;
        float Padding0;
        glm::vec3 LightDir;
        float Padding1;
        glm::vec4 LightColor;
        float Ambient;
        float Padding2[3];
    };
    static_assert(sizeof(PreviewCB) % 16 == 0, "PreviewCB must be 16-byte aligned");

    nvrhi::BufferHandle m_ConstantBuffer;
    nvrhi::CommandListHandle m_CommandList;
};
