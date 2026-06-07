#pragma once

#include <nvrhi/nvrhi.h>
#include <vector>
#include <cstdint>

#include "Engine.h"
#include "ApplicationContext.h"

// MaterialSystem manages GPU material resources (textures, samplers)
// and provides lookup by MaterialHandle/MaterialId
class ENGINE_API MaterialSystem
{
public:
    MaterialSystem() = default;
    ~MaterialSystem() = default;

    // Initialize the system with device reference and default resources
    void Initialize(nvrhi::IDevice* device, const nvrhi::TextureHandle &missingMaterial, const nvrhi::SamplerHandle &defaultSampler);

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

    static constexpr uint64_t MissingMaterial = { 0 }; // Reserved default mesh handle

    MaterialResources GetMaterialResources(uint64_t materialId) const;

    // Query material count (for UI/editor purposes)
    uint32_t GetMaterialCount() const;

    // Cleanup all resources
    void Shutdown();

    void SetDevice(nvrhi::IDevice* device) { m_Device = device; }

    // Hot-swap support: release GPU textures but keep entries + CPU caches.
    void DestroyGpuResources();
    // Hot-swap support: rebuild textures from CPU caches against m_Device.
    // newMissing / newSampler replace the previous default resources.
    // Returns false if any material failed (that slot falls back to missing).
    bool RecreateGpuResources(const nvrhi::TextureHandle& newMissing,
                              const nvrhi::SamplerHandle& newSampler);

private:
    struct MaterialEntry {
        nvrhi::TextureHandle texture;
        nvrhi::SamplerHandle sampler;

        // CPU-side copy retained for hot-swap. If usesMissingTexture is true,
        // this entry points at the shared missing texture and has no pixels.
        std::vector<uint32_t> cpuPixels;
        uint32_t width = 0;
        uint32_t height = 0;
        bool usesMissingTexture = false;
    };

    nvrhi::IDevice* m_Device = nullptr;
    nvrhi::TextureHandle m_MissingMaterial;
    nvrhi::SamplerHandle m_DefaultSampler;
    std::vector<MaterialEntry> m_Materials;
};
