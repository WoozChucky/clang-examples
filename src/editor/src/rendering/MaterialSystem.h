#pragma once

#include <nvrhi/nvrhi.h>
#include <vector>
#include <cstdint>

#include "ApplicationContext.h"

// MaterialSystem manages GPU material resources (textures, samplers)
// and provides lookup by MaterialHandle/MaterialId
class MaterialSystem
{
public:
    MaterialSystem() = default;
    ~MaterialSystem() = default;

    // Initialize the system with device reference and default resources
    void Initialize(nvrhi::IDevice* device, nvrhi::TextureHandle defaultWhite, nvrhi::SamplerHandle defaultSampler);

    // Upload material/texture data and return a handle
    // If texture data is null, uses default white texture
    // Returns MaterialHandle with Index = UINT32_MAX on failure
    MaterialHandle AddMaterial(const uint32_t* textureRgba8, uint32_t texWidth, uint32_t texHeight);

    // Query GPU resources by material ID
    struct MaterialResources {
        nvrhi::TextureHandle texture;
        nvrhi::SamplerHandle sampler;
        bool valid = false;
    };

    MaterialResources GetMaterialResources(uint32_t materialId) const;

    // Query material count (for UI/editor purposes)
    uint32_t GetMaterialCount() const;

    // Cleanup all resources
    void Shutdown();

private:
    struct MaterialEntry {
        nvrhi::TextureHandle texture;
        nvrhi::SamplerHandle sampler;
    };

    nvrhi::IDevice* m_Device = nullptr;
    nvrhi::TextureHandle m_DefaultWhite;
    nvrhi::SamplerHandle m_DefaultSampler;
    std::vector<MaterialEntry> m_Materials;
};
