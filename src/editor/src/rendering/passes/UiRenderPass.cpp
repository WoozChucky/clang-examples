#include "UiRenderPass.h"

#include "Renderer.h"
#include "nvrhi/utils.h"
#include <glm/gtc/matrix_transform.hpp>

inline auto QUAD_VS_HLSL = R"(
// UI Quad Vertex Shader (instanced)
// Transforms 2D quad vertices (in pixels) using per-instance transform read from a StructuredBuffer
// and an orthographic matrix. Also applies per-instance UV scale/offset and color.

[[vk::binding(0, 0)]] cbuffer UIFrame : register(b0)
{
    float4x4 uOrtho;        // orthographic projection to clip space
};

// Per-instance rendering flags (bitmask)
// bit 0 (1): SAMPLE_TEXTURE — sample atlas in PS; otherwise output solid color
struct UIInstance
{
    float4x4 Transform; // per-instance 2D transform (pos/scale/rotation in pixels)
    float4   Color;     // per-instance multiplicative color (rgba)
    float4   UVRect;    // xy = uvScale, zw = uvOffset (normalized)
    uint     Flags;     // bitmask of UI options (see above)
    uint3    _pad;      // padding to keep 16-byte alignment for structure stride
};

[[vk::binding(2, 0)]] StructuredBuffer<UIInstance> gUIInstances : register(t2);

struct VSIn
{
    float2 Position : POSITION;   // local quad position in pixels (e.g., (0,0)-(w,h))
    float2 UV       : TEXCOORD0;  // base UV in [0..1]
};

struct VSOut
{
    float4 PosH     : SV_POSITION;
    float2 UV       : TEXCOORD0;
    float4 Color    : COLOR0;
    nointerpolation uint Flags : TEXCOORD1;
};

VSOut main_vs(VSIn vin, uint instId : SV_InstanceID)
{
    UIInstance inst = gUIInstances[instId];

    float4 lp = float4(vin.Position, 0.0f, 1.0f);
    float4 wp = mul(inst.Transform, lp);

    VSOut o;
    o.PosH = mul(uOrtho, wp);
    o.UV = vin.UV * inst.UVRect.xy + inst.UVRect.zw; // scale + offset into atlas
    o.Color = inst.Color;
    o.Flags = inst.Flags;
    return o;
}
)";

inline auto QUAD_PS_HLSL = R"(
// UI Quad Pixel Shader
// Supports two modes controlled by per-instance Flags (see VS):
// - bit 0 (SAMPLE_TEXTURE): sample atlas (R channel as coverage) and modulate with Color
// - bit 0 off: output solid Color as-is

[[vk::binding(1, 0)]] Texture2D    uTexture : register(t1);
[[vk::binding(3, 0)]] SamplerState uSampler : register(s3);

static const uint UI_OPT_SAMPLE_TEXTURE = 1u << 0;

struct PSIn
{
    float4 PosH  : SV_POSITION;
    float2 UV    : TEXCOORD0;
    float4 Color : COLOR0;
    nointerpolation uint Flags : TEXCOORD1;
};

float4 main_ps(PSIn pin) : SV_Target
{
    if ((pin.Flags & UI_OPT_SAMPLE_TEXTURE) != 0u)
    {
        float4 texel = uTexture.Sample(uSampler, pin.UV);
        float alpha = texel.r; // R8_UNORM font atlas stores coverage in .r
        return float4(pin.Color.rgb, pin.Color.a * alpha);
    }
    else
    {
        return pin.Color;
    }
}
)";

struct UIVertex
{
    float x, y;   // position in pixels
    float u, v;   // texture coordinates
};

struct UIFrameCBData
{
    glm::mat4 Ortho;
};
static_assert(sizeof(UIFrameCBData) % 16 == 0);

struct UIInstanceCPU
{
    glm::mat4 Transform;
    glm::vec4 Color;
    glm::vec4 UVRect; // (scaleX, scaleY, offsetX, offsetY)
    uint32_t Flags;   // bitmask: e.g., 1<<0 = SAMPLE_TEXTURE
    uint32_t _pad[3]; // padding to maintain 16-byte alignment
};
static_assert(sizeof(UIInstanceCPU) % 16 == 0);

const FontManager::FontKey FontManager::DEFAULT_FONT = {"assets/LiberationSans-Regular.ttf", 16};

bool UiRenderPass::Initialize(nvrhi::IDevice *device, Renderer *renderer) {
    m_Device = device;
    m_Renderer = renderer;

    if (!m_Device || !m_Renderer) {
        SM_ERROR("PrimitiveRenderPass::Initialize - Invalid device or renderer");
        return false;
    }

    m_MaxInstances = 16384;

    // Compile shaders
    m_VS = m_Renderer->CreateShader(nvrhi::ShaderType::Vertex, QUAD_VS_HLSL, 0, "main_vs", "vs_6_1");
    m_PS = m_Renderer->CreateShader(nvrhi::ShaderType::Pixel, QUAD_PS_HLSL, 0, "main_ps", "ps_6_1");


    SM_ASSERT(m_VS && m_PS, "Failed to create UI shaders");

    // Create static quad VB/IB
    const UIVertex quadVerts[4] = {
        // x, y, u, v
        { 0.0f,  0.0f, 0.0f, 0.0f },
        { 1.0f,  0.0f, 1.0f, 0.0f },
        { 1.0f,  1.0f, 1.0f, 1.0f },
        { 0.0f,  1.0f, 0.0f, 1.0f },
    };
    const uint16_t quadIdx[6] = { 0, 1, 2, 0, 2, 3 };

    auto cl = m_Device->createCommandList();
    cl->open();

    // VB
    nvrhi::BufferDesc vbDesc;
    vbDesc.byteSize = sizeof(quadVerts);
    vbDesc.isVertexBuffer = true;
    vbDesc.debugName = "UIRenderPass VertexBuffer";
    vbDesc.initialState = nvrhi::ResourceStates::CopyDest;
    m_VertexBuffer = m_Device->createBuffer(vbDesc);

    cl->beginTrackingBufferState(m_VertexBuffer, nvrhi::ResourceStates::CopyDest);
    cl->writeBuffer(m_VertexBuffer, quadVerts, sizeof(quadVerts));
    cl->setPermanentBufferState(m_VertexBuffer, nvrhi::ResourceStates::VertexBuffer);

    // IB
    nvrhi::BufferDesc ibDesc;
    ibDesc.byteSize = sizeof(quadIdx);
    ibDesc.isIndexBuffer = true;
    ibDesc.debugName = "UIRenderPass IndexBuffer";
    ibDesc.initialState = nvrhi::ResourceStates::CopyDest;
    m_IndexBuffer = m_Device->createBuffer(ibDesc);

    cl->beginTrackingBufferState(m_IndexBuffer, nvrhi::ResourceStates::CopyDest);
    cl->writeBuffer(m_IndexBuffer, quadIdx, sizeof(quadIdx));
    cl->setPermanentBufferState(m_IndexBuffer, nvrhi::ResourceStates::IndexBuffer);

    m_IndexCount = 6;

    // Per-frame CB (uOrtho)
    m_PerFrameConstantBuffer = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(UIFrameCBData), "UIRenderPass FrameConstantBuffer")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer).setKeepInitialState(true));

    // Instance buffer (StructuredBuffer SRV)
    {
        nvrhi::BufferDesc instDesc;
        instDesc.debugName = "UIRenderPass InstanceBuffer";
        instDesc.byteSize = m_MaxInstances * sizeof(UIInstanceCPU);
        instDesc.structStride = sizeof(UIInstanceCPU);
        instDesc.initialState = nvrhi::ResourceStates::CopyDest; // upload then use as SRV
        instDesc.isVertexBuffer = false;
        instDesc.isIndexBuffer = false;
        instDesc.isConstantBuffer = false;
        instDesc.canHaveUAVs = false;
        instDesc.setInitialState(nvrhi::ResourceStates::CopyDest);
        instDesc.setKeepInitialState(true);
        m_InstanceBuffer = m_Device->createBuffer(instDesc);
    }

    // Input layout: POSITION (RG32_FLOAT), TEXCOORD (RG32_FLOAT)
    nvrhi::VertexAttributeDesc uiAttrs[] = {
        nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RG32_FLOAT)
            .setOffset(offsetof(UIVertex, x)).setBufferIndex(0).setElementStride(sizeof(UIVertex)),
        nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT)
            .setOffset(offsetof(UIVertex, u)).setBufferIndex(0).setElementStride(sizeof(UIVertex)),
    };
    m_InputLayout = m_Device->createInputLayout(uiAttrs, std::size(uiAttrs), m_VS);

    // Create binding layout for UI pass: b0 (PerFrame), t0 (Font texture), t1 (Instance buffer SRV), s0 (Sampler)
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::ConstantBuffer(0),  // b0
        nvrhi::BindingLayoutItem::Texture_SRV(1),     // t0
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2), // t1
        nvrhi::BindingLayoutItem::Sampler(3)          // s0
    };
    nvrhi::VulkanBindingOffsets& offsets =
        nvrhi::VulkanBindingOffsets{}.setConstantBufferOffset(0).setShaderResourceOffset(0).setSamplerOffset(0).setUnorderedAccessViewOffset(0);

    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        layoutDesc.setBindingOffsets(offsets);
    }
    m_BindingLayout = m_Device->createBindingLayout(layoutDesc);

    cl->close();
    m_Device->executeCommandList(cl);

    if (!m_FontManager.LoadAtlas(FontManager::DEFAULT_FONT.Path.c_str(), FontManager::DEFAULT_FONT.Size, m_Device)) {
        SM_ERROR("Failed to default font");
        return false;
    }

    // Provide UI resources to FontManager so it can create binding sets for any atlas
    m_FontManager.SetUIResources(m_BindingLayout, m_PerFrameConstantBuffer, m_InstanceBuffer);

    // Create UI binding set for the default font atlas
    if (auto* atlas = m_FontManager.GetAtlas(FontManager::DEFAULT_FONT, m_Device)) {
        m_FontManager.CreateUIBindingSet(*atlas);
        m_BindingSet = atlas->uiBindingSet;
    }

    return true;
}

void UiRenderPass::Render(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *frameBuffer,
    SimulationSnapshot &snapshot, const ECS* world, double deltaTime, FrameAllocator* frameAllocator) {

    if (!m_Pipeline) {
        const auto fbi = frameBuffer->getFramebufferInfo();
        nvrhi::GraphicsPipelineDesc uiDesc;
        uiDesc.VS = m_VS;
        uiDesc.PS = m_PS;
        uiDesc.inputLayout = m_InputLayout;
        uiDesc.bindingLayouts = { m_BindingLayout };
        uiDesc.primType = nvrhi::PrimitiveType::TriangleList;
        // UI states
        uiDesc.renderState.depthStencilState.depthTestEnable = false;
        uiDesc.renderState.depthStencilState.stencilEnable = false;
        // Enable straight alpha blending for UI/text using setter API
        nvrhi::BlendState::RenderTarget rt;
        rt.setBlendEnable(true)
          .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
          .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
          .setBlendOp(nvrhi::BlendOp::Add)
          .setSrcBlendAlpha(nvrhi::BlendFactor::One)
          .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
          .setBlendOpAlpha(nvrhi::BlendOp::Add)
          .setColorWriteMask(nvrhi::ColorMask::All);
        uiDesc.renderState.blendState.setRenderTarget(0, rt);

        m_Pipeline = m_Device->createGraphicsPipeline(uiDesc, fbi);
    }

    commandList->beginMarker("UIRenderPass");

    if (m_PerFrameConstantBuffer) {
        // UI projection/view come from the ECS world snapshot (UICameraComponent),
        // kept in sync with the window each tick by the game thread. This is the
        // same window-dims ortho convention the manual build used previously:
        // (0,0) top-left -> (-1,+1), (w,h) -> (+1,-1).
        glm::mat4 uiProj(1.0f), uiView(1.0f);
        if (const auto* ui = world ? world->GetSingleton<UICameraComponent>() : nullptr) {
            uiProj = ui->Projection;
            uiView = ui->View;
        }

        UIFrameCBData uiFrame{};
        uiFrame.Ortho = uiProj * uiView;
        commandList->writeBuffer(m_PerFrameConstantBuffer, &uiFrame, sizeof(uiFrame));
    }

    nvrhi::GraphicsState state;
    state.pipeline = m_Pipeline;
    state.framebuffer = frameBuffer;
    state.viewport.addViewportAndScissorRect(frameBuffer->getFramebufferInfo().getViewport());
    state.vertexBuffers = { nvrhi::VertexBufferBinding(m_VertexBuffer, 0, 0) };
    state.indexBuffer = nvrhi::IndexBufferBinding(m_IndexBuffer, nvrhi::Format::R16_UINT, 0);

    if (world) {

        // 1) Gather unique font sizes used by text entities
        std::vector<size_t> usedFontSizes;
        for (EntityId entity : world->View<TransformComponent, TextComponent>()) {
            const auto* text = world->GetComponent<TextComponent>(entity);
            if (text) {
                const size_t fontSize = text->FontSize;
                bool found = false;
                for (size_t fs : usedFontSizes) {
                    if (fs == fontSize) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    usedFontSizes.push_back(fontSize);
                }
            }
        }

        // 2) For each unique font size, render all text using that atlas
        for (size_t fontSize : usedFontSizes) {
            // Get or load atlas for this font size
            auto* atlas = m_FontManager.GetAtlas({FontManager::DEFAULT_FONT.Path, fontSize}, m_Device, commandList);
            if (!atlas || !atlas->texture) continue;

            // Ensure this atlas has a UI binding set
            if (!atlas->uiBindingSet) {
                m_FontManager.CreateUIBindingSet(*atlas);
            }
            if (!atlas->uiBindingSet) continue; // Skip if we couldn't create binding set

            const auto aw = static_cast<float>(atlas->width);
            const auto ah = static_cast<float>(atlas->height);

            // Count glyphs for this font size
            uint32_t glyphCount = 0;
            for (EntityId entity : world->View<TransformComponent, TextComponent>()) {
                const auto* text = world->GetComponent<TextComponent>(entity);
                if (text && text->FontSize == fontSize) {
                    glyphCount += static_cast<uint32_t>(text->Text.length());
                }
            }

            if (glyphCount == 0) continue;
            glyphCount = std::min(glyphCount, m_MaxInstances);

            // Allocate glyph instances for this font
            auto* glyphInstances = frameAllocator->AllocateArray<UIInstanceCPU>(glyphCount);
            uint32_t out = 0;

            // Generate instances for all text entities using this font size
            for (EntityId entity : world->View<TransformComponent, TextComponent>()) {
                auto* transform = world->GetComponent<TransformComponent>(entity);
                auto* text = world->GetComponent<TextComponent>(entity);

                if (transform && text && text->FontSize == fontSize) {
                    const std::string& str = text->Text;

                    // Build entity transform matrix (Translation * Rotation * Scale)
                    // For 2D UI, we primarily care about Z-axis rotation
                    glm::mat4 T = glm::translate(glm::mat4(1.0f), transform->Position);
                    glm::mat4 R = glm::rotate(glm::mat4(1.0f), transform->Rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));
                    glm::mat4 S = glm::scale(glm::mat4(1.0f), transform->Scale);
                    glm::mat4 entityTransform = T * R * S;

                    // Start pen at origin (will be transformed by entity transform)
                    glm::vec2 pen = glm::vec2(0.0f, 0.0f);

                    // Iterate through each character
                    for (size_t k = 0; k < str.length() && out < glyphCount; ++k) {
                        const auto c = static_cast<unsigned char>(str[k]);
                        if (c >= 128u) continue; // Skip non-ASCII characters

                        const Glyph& g = atlas->glyphs[c];

                        // Calculate glyph position in local space (relative to text origin)
                        glm::vec2 localPos;
                        localPos.x = pen.x + g.offset.x;
                        localPos.y = pen.y - g.offset.y;
                        const glm::vec2 size = g.size;

                        // Create local glyph transform (scale and position in local space)
                        auto localGlyphTransform = glm::mat4(1.0f);
                        localGlyphTransform[0][0] = size.x;
                        localGlyphTransform[1][1] = size.y;
                        localGlyphTransform[3][0] = localPos.x;
                        localGlyphTransform[3][1] = localPos.y;

                        // Compose: final transform = entity transform * local glyph transform
                        UIInstanceCPU inst{};
                        inst.Transform = entityTransform * localGlyphTransform;
                        inst.Color = text->Color;
                        inst.Flags = 1u << 0; // SAMPLE_TEXTURE

                        // Calculate UV coordinates
                        const auto uvScale = glm::vec2(g.size.x / aw, g.size.y / ah);
                        const auto uvOffset = glm::vec2(g.textureCoords.x / aw, g.textureCoords.y / ah);
                        inst.UVRect = glm::vec4(uvScale.x, uvScale.y, uvOffset.x, uvOffset.y);

                        glyphInstances[out++] = inst;

                        // Advance pen in local space
                        pen.x += g.advance.x;
                    }
                }
            }

            // Upload and draw for this font size
            if (out > 0) {
                commandList->writeBuffer(m_InstanceBuffer, glyphInstances, out * sizeof(UIInstanceCPU));
                state.bindings = { atlas->uiBindingSet };
                commandList->setGraphicsState(state);
                nvrhi::DrawArguments uiDrawArgs;
                uiDrawArgs.vertexCount = m_IndexCount;
                uiDrawArgs.instanceCount = out;
                uiDrawArgs.startIndexLocation = 0;
                uiDrawArgs.startVertexLocation = 0;
                commandList->drawIndexed(uiDrawArgs);
            }

            // Note: No need to delete glyphInstances - frame allocator owns memory and will reset at end of frame
        }
    }

    commandList->endMarker();
}

void UiRenderPass::Shutdown() {
    // Clear all pipeline objects
    m_Pipeline = nullptr;
    m_BindingSet = nullptr;
    m_BindingLayout = nullptr;
    m_InputLayout = nullptr;
    m_IndexCount = 0;
    m_MaxInstances = 0;
    m_PerFrameConstantBuffer = nullptr;
    m_InstanceBuffer = nullptr;
    m_VertexBuffer = nullptr;
    m_IndexBuffer = nullptr;
    m_VS = nullptr;
    m_PS = nullptr;

    m_Device = nullptr;
}

void UiRenderPass::OnResize(uint32_t width, uint32_t height) {
    // Invalidate pipeline on resize so it gets recreated with new framebuffer info
    m_Pipeline = nullptr;
}
