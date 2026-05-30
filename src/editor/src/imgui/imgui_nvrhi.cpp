#include "imgui_nvrhi.h"

#include <windows.h>

#include <print>

#include "shader/ShaderCompiler.h"

inline auto IMGUI_VS_HLSL = R"(
struct VS_INPUT
{
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

struct PS_INPUT
{
    float4 out_pos : SV_POSITION;
    float4 out_col : COLOR0;
    float2 out_uv : TEXCOORD0;
};

struct PC
{
    float2 invDisplaySize;
};
[[vk::push_constant]] ConstantBuffer<PC> g_PC : register(b0, space0);

PS_INPUT main(VS_INPUT input)
{
    PS_INPUT output;
    output.out_pos.xy = input.pos.xy * g_PC.invDisplaySize * float2(2.0, -2.0) + float2(-1.0, 1.0);
    output.out_pos.zw = float2(0.0, 1.0);
    output.out_col = input.col;
    output.out_uv = input.uv;
    return output;
};
)";

inline auto IMGUI_PS_HLSL = R"(
struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};

Texture2D texture0 : register(t0);
sampler sampler0 : register(s1);

float4 main(PS_INPUT input) : SV_Target
{
    float4 out_col = input.col * texture0.Sample(sampler0, input.uv);
    return out_col;
}
)";

nvrhi::ShaderHandle CreateShader(RendererAPI api, nvrhi::IDevice* device, const char* entryName, const char* content, const nvrhi::ShaderDesc& desc)
{
    nvrhi::ShaderDesc descCopy = desc;
    descCopy.entryName = entryName;

    const char* targetName = nullptr;

    if (true)
    {
        targetName = "vs_6_1";
        if (desc.shaderType == nvrhi::ShaderType::Pixel)
            targetName = "ps_6_1";
    }

    std::string errors;
    auto [data] = CompileShader(api, content, descCopy.entryName.c_str(), targetName, errors);
    if (!errors.empty())
    {
        std::println(stderr, "Failed to compile ImGui shader: {}", errors);
        return nullptr;
    }

    const auto shader = device->createShader(descCopy, data.data(), data.size());

    data.clear();

    return shader;
}

bool ImGui_NVRHI::updateFontTexture()
{
    ImGuiIO& io = ImGui::GetIO();

    // If the font texture exists and is bound to ImGui, we're done.
    // Note: ImGui_Renderer will reset io.Fonts->TexRef when new fonts are added.
    if (fontTexture && io.Fonts->TexRef.GetTexID())
        return true;

    unsigned char *pixels;
    int width, height;

    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    if (!pixels)
        return false;

    nvrhi::TextureDesc textureDesc;
    textureDesc.width = width;
    textureDesc.height = height;
    textureDesc.format = nvrhi::Format::RGBA8_UNORM;
    textureDesc.debugName = "ImGui font texture";

    fontTexture = m_device->createTexture(textureDesc);

    if (fontTexture == nullptr)
        return false;

    m_commandList->open();

    m_commandList->beginTrackingTextureState(fontTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);

    m_commandList->writeTexture(fontTexture, 0, 0, pixels, width * 4);

    m_commandList->setPermanentTextureState(fontTexture, nvrhi::ResourceStates::ShaderResource);
    m_commandList->commitBarriers();

    m_commandList->close();
    m_device->executeCommandList(m_commandList);

    io.Fonts->TexRef = ImTextureRef(fontTexture.Get());

    return true;
}

bool ImGui_NVRHI::init(nvrhi::IDevice* device)
{
    m_device = device;
    const auto nvRhiApi = m_device->getGraphicsAPI();
    auto rendererApi = RendererAPI::DirectX12;
    if (nvRhiApi == nvrhi::GraphicsAPI::VULKAN)
        rendererApi = RendererAPI::Vulkan;

    m_commandList = m_device->createCommandList();

    vertexShader = CreateShader(rendererApi, device, "main", IMGUI_VS_HLSL, nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex));
    pixelShader = CreateShader(rendererApi, device, "main", IMGUI_PS_HLSL, nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel));

    if (!vertexShader || !pixelShader)
    {
        fprintf(stderr, "Failed to create an ImGUI shader");
        return false;
    }

    // create attribute layout object
    nvrhi::VertexAttributeDesc vertexAttribLayout[] = {
        { "POSITION", nvrhi::Format::RG32_FLOAT,  1, 0, offsetof(ImDrawVert,pos), sizeof(ImDrawVert), false },
        { "TEXCOORD", nvrhi::Format::RG32_FLOAT,  1, 0, offsetof(ImDrawVert,uv),  sizeof(ImDrawVert), false },
        { "COLOR",    nvrhi::Format::RGBA8_UNORM, 1, 0, offsetof(ImDrawVert,col), sizeof(ImDrawVert), false },
    };

    shaderAttribLayout = m_device->createInputLayout(vertexAttribLayout, sizeof(vertexAttribLayout) / sizeof(vertexAttribLayout[0]), vertexShader);

    // create PSO
    {
        nvrhi::BlendState blendState;
        blendState.targets[0].setBlendEnable(true)
            .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
            .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
            .setSrcBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
            .setDestBlendAlpha(nvrhi::BlendFactor::Zero);

        auto rasterState = nvrhi::RasterState()
            .setFillSolid()
            .setCullNone()
            .setScissorEnable(true)
            .setDepthClipEnable(true);

        auto depthStencilState = nvrhi::DepthStencilState()
            .disableDepthTest()
            .enableDepthWrite()
            .disableStencil()
            .setDepthFunc(nvrhi::ComparisonFunc::Always);

        nvrhi::RenderState renderState;
        renderState.blendState = blendState;
        renderState.depthStencilState = depthStencilState;
        renderState.rasterState = rasterState;

        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::All;
        layoutDesc.registerSpace = 0;
        layoutDesc.registerSpaceIsDescriptorSet = false;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::PushConstants(0, sizeof(float) * 2),
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::Sampler(1)
        };
        nvrhi::VulkanBindingOffsets& offsets =
            nvrhi::VulkanBindingOffsets{}.setConstantBufferOffset(0).setShaderResourceOffset(0).setSamplerOffset(0);

        if (m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
        {
            layoutDesc.setBindingOffsets(offsets);
        }

        bindingLayout = m_device->createBindingLayout(layoutDesc);

        basePSODesc.primType = nvrhi::PrimitiveType::TriangleList;
        basePSODesc.inputLayout = shaderAttribLayout;
        basePSODesc.VS = vertexShader;
        basePSODesc.PS = pixelShader;
        basePSODesc.renderState = renderState;
        basePSODesc.bindingLayouts = { bindingLayout };
    }

    {
        const auto desc = nvrhi::SamplerDesc()
            .setAllAddressModes(nvrhi::SamplerAddressMode::Wrap)
            .setAllFilters(true);

        fontSampler = m_device->createSampler(desc);

        if (fontSampler == nullptr)
            return false;
    }

    return true;
}

bool ImGui_NVRHI::reallocateBuffer(nvrhi::BufferHandle& buffer, size_t requiredSize, size_t reallocateSize, const bool indexBuffer)
{
    if (buffer == nullptr || size_t(buffer->getDesc().byteSize) < requiredSize)
    {
        nvrhi::BufferDesc desc;
        desc.byteSize = uint32_t(reallocateSize);
        desc.structStride = 0;
        desc.debugName = indexBuffer ? "ImGui index buffer" : "ImGui vertex buffer";
        desc.canHaveUAVs = false;
        desc.isVertexBuffer = !indexBuffer;
        desc.isIndexBuffer = indexBuffer;
        desc.isDrawIndirectArgs = false;
        desc.isVolatile = false;
        desc.initialState = indexBuffer ? nvrhi::ResourceStates::IndexBuffer : nvrhi::ResourceStates::VertexBuffer;
        desc.keepInitialState = true;

        buffer = m_device->createBuffer(desc);

        if (!buffer)
        {
            return false;
        }
    }

    return true;
}

nvrhi::IGraphicsPipeline* ImGui_NVRHI::getPSO(nvrhi::FramebufferInfo const& framebufferInfo)
{
    if (pso)
        return pso;

    pso = m_device->createGraphicsPipeline(basePSODesc, framebufferInfo);
    assert(pso);

    return pso;
}

nvrhi::IBindingSet* ImGui_NVRHI::getBindingSet(nvrhi::ITexture* texture)
{
    auto iter = bindingsCache.find(texture);
    if (iter != bindingsCache.end())
    {
        return iter->second;
    }

    nvrhi::BindingSetDesc desc;

    desc.bindings = {
        nvrhi::BindingSetItem::PushConstants(0, sizeof(float) * 2),
        nvrhi::BindingSetItem::Texture_SRV(0, texture),
        nvrhi::BindingSetItem::Sampler(1, fontSampler)
    };

    nvrhi::BindingSetHandle binding;
    binding = m_device->createBindingSet(desc, bindingLayout);
    assert(binding);

    bindingsCache[texture] = binding;
    return binding;
}

bool ImGui_NVRHI::updateGeometry(nvrhi::ICommandList* commandList)
{
    ImDrawData *drawData = ImGui::GetDrawData();

    // create/resize vertex and index buffers if needed
    if (!reallocateBuffer(vertexBuffer,
        drawData->TotalVtxCount * sizeof(ImDrawVert),
        (drawData->TotalVtxCount + 5000) * sizeof(ImDrawVert),
        false))
    {
        return false;
    }

    if (!reallocateBuffer(indexBuffer,
        drawData->TotalIdxCount * sizeof(ImDrawIdx),
        (drawData->TotalIdxCount + 5000) * sizeof(ImDrawIdx),
        true))
    {
        return false;
    }

    vtxBuffer.resize(vertexBuffer->getDesc().byteSize / sizeof(ImDrawVert));
    idxBuffer.resize(indexBuffer->getDesc().byteSize / sizeof(ImDrawIdx));

    // copy and convert all vertices into a single contiguous buffer
    ImDrawVert *vtxDst = &vtxBuffer[0];
    ImDrawIdx *idxDst = &idxBuffer[0];

    for(int n = 0; n < drawData->CmdListsCount; n++)
    {
        const ImDrawList *cmdList = drawData->CmdLists[n];

        memcpy(vtxDst, cmdList->VtxBuffer.Data, cmdList->VtxBuffer.Size * sizeof(ImDrawVert));
        memcpy(idxDst, cmdList->IdxBuffer.Data, cmdList->IdxBuffer.Size * sizeof(ImDrawIdx));

        vtxDst += cmdList->VtxBuffer.Size;
        idxDst += cmdList->IdxBuffer.Size;
    }

    commandList->writeBuffer(vertexBuffer, &vtxBuffer[0], vertexBuffer->getDesc().byteSize);
    commandList->writeBuffer(indexBuffer, &idxBuffer[0], indexBuffer->getDesc().byteSize);

    return true;
}

bool ImGui_NVRHI::render(nvrhi::IFramebuffer* framebuffer)
{
    ImDrawData *drawData = ImGui::GetDrawData();
    const auto& io = ImGui::GetIO();

    m_commandList->open();
    m_commandList->beginMarker("ImGUI");

    if (!updateGeometry(m_commandList))
    {
        m_commandList->close();
        return false;
    }

    // handle DPI scaling
    drawData->ScaleClipRects(io.DisplayFramebufferScale);

    float invDisplaySize[2] = { 1.f / io.DisplaySize.x, 1.f / io.DisplaySize.y };

    // set up graphics state
    nvrhi::GraphicsState drawState;

    drawState.framebuffer = framebuffer;
    assert(drawState.framebuffer);

    drawState.pipeline = getPSO(framebuffer->getFramebufferInfo());

    drawState.viewport.viewports.push_back(nvrhi::Viewport(io.DisplaySize.x * io.DisplayFramebufferScale.x,
                                           io.DisplaySize.y * io.DisplayFramebufferScale.y));
    drawState.viewport.scissorRects.resize(1);  // updated below

    nvrhi::VertexBufferBinding vbufBinding;
    vbufBinding.buffer = vertexBuffer;
    vbufBinding.slot = 0;
    vbufBinding.offset = 0;
    drawState.vertexBuffers.push_back(vbufBinding);

    drawState.indexBuffer.buffer = indexBuffer;
    drawState.indexBuffer.format = (sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT);
    drawState.indexBuffer.offset = 0;

    // render command lists
    int vtxOffset = 0;
    int idxOffset = 0;
    for(int n = 0; n < drawData->CmdListsCount; n++)
    {
        const ImDrawList *cmdList = drawData->CmdLists[n];
        for(int i = 0; i < cmdList->CmdBuffer.Size; i++)
        {
            const ImDrawCmd *pCmd = &cmdList->CmdBuffer[i];

            if (pCmd->UserCallback)
            {
                pCmd->UserCallback(cmdList, pCmd);
            } else {
                // Project scissor/clip rect. ImGui can emit fully-clipped draw commands
                // (degenerate clip rect); D3D12 forbids an empty scissor and the debug
                // layer breaks on DRAW_EMPTY_SCISSOR_RECTANGLE. Clamp min to >=0 and skip
                // the draw when the rect has no area (matches imgui_impl_dx12).
                int clipMinX = int(pCmd->ClipRect.x);
                int clipMinY = int(pCmd->ClipRect.y);
                int clipMaxX = int(pCmd->ClipRect.z);
                int clipMaxY = int(pCmd->ClipRect.w);
                if (clipMinX < 0) clipMinX = 0;
                if (clipMinY < 0) clipMinY = 0;

                if (clipMaxX > clipMinX && clipMaxY > clipMinY)
                {
                    drawState.bindings = { getBindingSet((nvrhi::ITexture*)pCmd->TexRef.GetTexID()) };
                    assert(drawState.bindings[0]);

                    drawState.viewport.scissorRects[0] = nvrhi::Rect(clipMinX, clipMaxX, clipMinY, clipMaxY);

                    nvrhi::DrawArguments drawArguments;
                    drawArguments.vertexCount = pCmd->ElemCount;
                    drawArguments.startIndexLocation = idxOffset;
                    drawArguments.startVertexLocation = vtxOffset;

                    m_commandList->setGraphicsState(drawState);
                    m_commandList->setPushConstants(invDisplaySize, sizeof(invDisplaySize));
                    m_commandList->drawIndexed(drawArguments);
                }
            }

            idxOffset += pCmd->ElemCount;
        }

        vtxOffset += cmdList->VtxBuffer.Size;
    }

    m_commandList->endMarker();
    m_commandList->close();
    m_device->executeCommandList(m_commandList);

    return true;
}

void ImGui_NVRHI::backBufferResizing()
{
    pso = nullptr;
}
