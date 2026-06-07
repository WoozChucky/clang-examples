#include "MaterialSystem.h"
#include "lib.h"
#include "AssetKey.h"

void MaterialSystem::Initialize(nvrhi::IDevice* device, const nvrhi::TextureHandle &missingMaterial, const nvrhi::SamplerHandle &defaultSampler)
{
    m_Device = device;
    m_MissingMaterial = missingMaterial;
    m_DefaultSampler = defaultSampler;
    m_Materials.clear();
    m_SlotByHandle.clear();

    // Create default fallback material at index 0 (magenta checkerboard texture, default sampler)
    // This ensures objects without materials can still render
    MaterialEntry defaultMaterial{};
    defaultMaterial.key = "";
    defaultMaterial.texture = m_MissingMaterial;
    defaultMaterial.sampler = m_DefaultSampler;
    defaultMaterial.usesMissingTexture = true;
    m_SlotByHandle[kMissingAssetHandle] = 0;
    m_Materials.push_back(defaultMaterial);

    SM_TRACE("MaterialSystem::Initialize: Created default material 0 with white texture");
}

MaterialHandle MaterialSystem::AddMaterial(std::string key, const uint32_t* textureRgba8, uint32_t texWidth, uint32_t texHeight)
{
    if (!m_Device)
    {
        SM_ERROR("MaterialSystem::AddMaterial: System not initialized");
        return MaterialHandle{ UINT64_MAX };
    }

    const uint64_t handle = AssetKeyHash(key);
    if (auto it = m_SlotByHandle.find(handle); it != m_SlotByHandle.end()) return MaterialHandle{ handle };

    MaterialEntry entry{};
    entry.key = std::move(key);
    entry.sampler = m_DefaultSampler;

    // If no texture data provided, use default white texture
    if (!textureRgba8 || texWidth == 0 || texHeight == 0)
    {
        entry.texture = m_MissingMaterial;
        entry.usesMissingTexture = true;
        const uint32_t slot = static_cast<uint32_t>(m_Materials.size());
        m_Materials.push_back(std::move(entry));
        m_SlotByHandle[handle] = slot;
        SM_TRACE("MaterialSystem::AddMaterial: '%s' -> handle %llu (slot %u)", m_Materials[slot].key.c_str(), (unsigned long long)handle, slot);
        return MaterialHandle{ handle };
    }

    // Retain CPU-side copy for hot-swap replay.
    entry.width = texWidth;
    entry.height = texHeight;
    entry.cpuPixels.assign(textureRgba8, textureRgba8 + (static_cast<size_t>(texWidth) * texHeight));
    entry.usesMissingTexture = false;

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
        return MaterialHandle{ UINT64_MAX };
    }

    cl->beginTrackingTextureState(entry.texture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    cl->writeTexture(entry.texture, 0, 0, textureRgba8, texWidth * 4);
    cl->setPermanentTextureState(entry.texture, nvrhi::ResourceStates::ShaderResource);
    cl->commitBarriers();

    // Execute upload commands
    cl->close();
    m_Device->executeCommandList(cl);

    // Store material entry and return handle
    const uint32_t slot = static_cast<uint32_t>(m_Materials.size());
    m_Materials.push_back(std::move(entry));
    m_SlotByHandle[handle] = slot;
    SM_TRACE("MaterialSystem::AddMaterial: '%s' -> handle %llu (slot %u)", m_Materials[slot].key.c_str(), (unsigned long long)handle, slot);
    return MaterialHandle{ handle };
}

int32_t MaterialSystem::SlotForHandle(uint64_t handle) const {
    auto it = m_SlotByHandle.find(handle);
    return it == m_SlotByHandle.end() ? -1 : static_cast<int32_t>(it->second);
}

MaterialSystem::MaterialResources MaterialSystem::GetMaterialResources(uint64_t materialId) const
{
    MaterialResources resources{};
    const int32_t slot = SlotForHandle(materialId);
    if (slot < 0) { SM_WARN("MaterialSystem::GetMaterialResources: unknown material handle %llu", (unsigned long long)materialId); return resources; }
    const MaterialEntry& entry = m_Materials[slot];
    resources.texture = entry.texture;
    resources.sampler = entry.sampler;
    resources.valid = true;
    return resources;
}

std::string MaterialSystem::KeyForHandle(uint64_t handle) const {
    const int32_t slot = SlotForHandle(handle);
    return slot < 0 ? std::string() : m_Materials[slot].key;
}

std::vector<std::pair<uint64_t, std::string>> MaterialSystem::GetAssetList() const {
    std::vector<std::pair<uint64_t, std::string>> out;
    out.reserve(m_SlotByHandle.size());
    for (const auto& [h, slot] : m_SlotByHandle) out.emplace_back(h, m_Materials[slot].key);
    return out;
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
    m_SlotByHandle.clear();
    m_MissingMaterial = nullptr;
    m_DefaultSampler = nullptr;
    m_Device = nullptr;
}

void MaterialSystem::DestroyGpuResources()
{
    for (auto& entry : m_Materials)
    {
        entry.texture = nullptr;
        entry.sampler = nullptr;
    }
    m_MissingMaterial = nullptr;
    m_DefaultSampler = nullptr;
}

bool MaterialSystem::RecreateGpuResources(const nvrhi::TextureHandle& newMissing,
                                          const nvrhi::SamplerHandle& newSampler)
{
    if (!m_Device)
    {
        SM_ERROR("MaterialSystem::RecreateGpuResources: no device");
        return false;
    }

    m_MissingMaterial = newMissing;
    m_DefaultSampler = newSampler;

    bool allOk = true;
    for (size_t i = 0; i < m_Materials.size(); ++i)
    {
        MaterialEntry& entry = m_Materials[i];
        entry.sampler = m_DefaultSampler;

        if (entry.usesMissingTexture)
        {
            entry.texture = m_MissingMaterial;
            continue;
        }

        if (entry.cpuPixels.empty() || entry.width == 0 || entry.height == 0)
        {
            SM_WARN("MaterialSystem::RecreateGpuResources: material %zu has no CPU cache; using missing", i);
            entry.texture = m_MissingMaterial;
            allOk = false;
            continue;
        }

        nvrhi::TextureDesc td;
        td.debugName = "MaterialSystem Texture " + std::to_string(i);
        td.width = entry.width;
        td.height = entry.height;
        td.depth = 1;
        td.arraySize = 1;
        td.mipLevels = 1;
        td.sampleCount = 1;
        td.dimension = nvrhi::TextureDimension::Texture2D;
        td.format = nvrhi::Format::RGBA8_UNORM;
        td.isShaderResource = true;
        entry.texture = m_Device->createTexture(td);
        if (!entry.texture)
        {
            SM_ERROR("MaterialSystem::RecreateGpuResources: material %zu texture failed; using missing", i);
            entry.texture = m_MissingMaterial;
            allOk = false;
            continue;
        }

        auto cl = m_Device->createCommandList(
            nvrhi::CommandListParameters().setQueueType(nvrhi::CommandQueue::Graphics));
        cl->open();
        cl->beginTrackingTextureState(entry.texture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
        cl->writeTexture(entry.texture, 0, 0, entry.cpuPixels.data(), entry.width * 4);
        cl->setPermanentTextureState(entry.texture, nvrhi::ResourceStates::ShaderResource);
        cl->commitBarriers();
        cl->close();
        m_Device->executeCommandList(cl);
    }

    SM_TRACE("MaterialSystem::RecreateGpuResources: rebuilt %zu materials", m_Materials.size());
    return allOk;
}
