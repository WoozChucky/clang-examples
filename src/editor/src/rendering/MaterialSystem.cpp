#include "MaterialSystem.h"
#include "lib.h"

void MaterialSystem::Initialize(nvrhi::IDevice* device, nvrhi::TextureHandle defaultWhite, nvrhi::SamplerHandle defaultSampler)
{
    m_Device = device;
    m_DefaultWhite = defaultWhite;
    m_DefaultSampler = defaultSampler;
    m_Materials.clear();

    // Create default fallback material at index 0 (white texture, default sampler)
    // This ensures objects without materials can still render
    MaterialEntry defaultMaterial{};
    defaultMaterial.texture = m_DefaultWhite;
    defaultMaterial.sampler = m_DefaultSampler;
    m_Materials.push_back(defaultMaterial);

    SM_TRACE("MaterialSystem::Initialize: Created default material 0 with white texture");
}

MaterialHandle MaterialSystem::AddMaterial(const uint32_t* textureRgba8, uint32_t texWidth, uint32_t texHeight)
{
    if (!m_Device)
    {
        SM_ERROR("MaterialSystem::AddMaterial: System not initialized");
        return MaterialHandle{ UINT32_MAX };
    }

    MaterialEntry entry{};
    entry.sampler = m_DefaultSampler;

    // If no texture data provided, use default white texture
    if (!textureRgba8 || texWidth == 0 || texHeight == 0)
    {
        entry.texture = m_DefaultWhite;
        const uint32_t materialId = static_cast<uint32_t>(m_Materials.size());
        m_Materials.push_back(entry);
        SM_TRACE("MaterialSystem::AddMaterial: Created material %u with default white texture", materialId);
        return MaterialHandle{ materialId };
    }

    // Create command list for upload
    auto cl = m_Device->createCommandList(nvrhi::CommandListParameters().setQueueType(nvrhi::CommandQueue::Graphics));
    cl->open();

    // Create and upload texture
    nvrhi::TextureDesc td;
    td.debugName = "MaterialSystem Texture " + std::to_string(m_Materials.size());
    td.width = texWidth;
    td.height = texHeight;
    td.depth = 1;
    td.arraySize = 1;
    td.mipLevels = 1;
    td.sampleCount = 1;
    td.dimension = nvrhi::TextureDimension::Texture2D;
    td.format = nvrhi::Format::RGBA8_UNORM;
    td.isShaderResource = true;

    // TODO: Add support for generating mipmaps if needed

    entry.texture = m_Device->createTexture(td);

    if (!entry.texture)
    {
        SM_ERROR("MaterialSystem::AddMaterial: Failed to create texture");
        cl->close();
        return MaterialHandle{ UINT32_MAX };
    }

    cl->beginTrackingTextureState(entry.texture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    cl->writeTexture(entry.texture, 0, 0, textureRgba8, texWidth * 4);
    cl->setPermanentTextureState(entry.texture, nvrhi::ResourceStates::ShaderResource);
    cl->commitBarriers();

    // Execute upload commands
    cl->close();
    m_Device->executeCommandList(cl);

    // Store material entry and return handle
    const uint32_t materialId = static_cast<uint32_t>(m_Materials.size());
    m_Materials.push_back(entry);

    SM_TRACE("MaterialSystem::AddMaterial: Created material %u with texture %ux%u",
             materialId, texWidth, texHeight);

    return MaterialHandle{ materialId };
}

MaterialSystem::MaterialResources MaterialSystem::GetMaterialResources(uint32_t materialId) const
{
    MaterialResources resources{};

    if (materialId >= m_Materials.size())
    {
        SM_WARN("MaterialSystem::GetMaterialResources: Invalid material ID %u", materialId);
        return resources;
    }

    const MaterialEntry& entry = m_Materials[materialId];
    resources.texture = entry.texture;
    resources.sampler = entry.sampler;
    resources.valid = true;

    return resources;
}

uint32_t MaterialSystem::GetMaterialCount() const
{
    return static_cast<uint32_t>(m_Materials.size());
}

void MaterialSystem::Shutdown()
{
    for (auto& entry : m_Materials)
    {
        entry.texture = nullptr;
        entry.sampler = nullptr;
    }
    m_Materials.clear();
    m_DefaultWhite = nullptr;
    m_DefaultSampler = nullptr;
    m_Device = nullptr;
}
