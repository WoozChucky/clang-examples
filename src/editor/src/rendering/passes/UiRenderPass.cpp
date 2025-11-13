#include "UiRenderPass.h"

#include "Renderer.h"
#include "nvrhi/utils.h"

inline auto QUAD_VS_HLSL = R"(
// UI Quad Vertex Shader (instanced)
// Transforms 2D quad vertices (in pixels) using per-instance transform read from a StructuredBuffer
// and an orthographic matrix. Also applies per-instance UV scale/offset and color.

cbuffer UIFrame : register(b0)
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

StructuredBuffer<UIInstance> gUIInstances : register(t1);

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

Texture2D    uTexture : register(t0);
SamplerState uSampler : register(s0);

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
    m_VS = m_Renderer->CreateShader(nvrhi::ShaderType::Vertex, QUAD_VS_HLSL, 0, "main_vs", "vs_5_1");
    m_PS = m_Renderer->CreateShader(nvrhi::ShaderType::Pixel, QUAD_PS_HLSL, 0, "main_ps", "ps_5_1");


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
    vbDesc.debugName = "UIQuadVB";
    vbDesc.initialState = nvrhi::ResourceStates::CopyDest;
    m_VertexBuffer = m_Device->createBuffer(vbDesc);

    cl->beginTrackingBufferState(m_VertexBuffer, nvrhi::ResourceStates::CopyDest);
    cl->writeBuffer(m_VertexBuffer, quadVerts, sizeof(quadVerts));
    cl->setPermanentBufferState(m_VertexBuffer, nvrhi::ResourceStates::VertexBuffer);

    // IB
    nvrhi::BufferDesc ibDesc;
    ibDesc.byteSize = sizeof(quadIdx);
    ibDesc.isIndexBuffer = true;
    ibDesc.debugName = "UIQuadIB";
    ibDesc.initialState = nvrhi::ResourceStates::CopyDest;
    m_IndexBuffer = m_Device->createBuffer(ibDesc);

    cl->beginTrackingBufferState(m_IndexBuffer, nvrhi::ResourceStates::CopyDest);
    cl->writeBuffer(m_IndexBuffer, quadIdx, sizeof(quadIdx));
    cl->setPermanentBufferState(m_IndexBuffer, nvrhi::ResourceStates::IndexBuffer);

    m_IndexCount = 6;

    // Per-frame CB (uOrtho)
    m_PerFrameConstantBuffer = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(UIFrameCBData), "UIFrameCB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer).setKeepInitialState(true));

    // Instance buffer (StructuredBuffer SRV)
    {
        nvrhi::BufferDesc instDesc;
        instDesc.debugName = "UIInstanceBuffer";
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
        nvrhi::BindingLayoutItem::Texture_SRV(0),     // t0
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1), // t1
        nvrhi::BindingLayoutItem::Sampler(0)          // s0
    };
    m_BindingLayout = m_Device->createBindingLayout(layoutDesc);

    cl->close();
    m_Device->executeCommandList(cl);

    if (!m_FontManager.LoadAtlas(FontManager::DEFAULT_FONT.Path.c_str(), FontManager::DEFAULT_FONT.Size, m_Device)) {
        SM_ERROR("Failed to default font");
        return false;
    }

    if (auto* atlas = m_FontManager.GetAtlas(FontManager::DEFAULT_FONT)) {
        nvrhi::BindingSetDesc bsd;
        bsd.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_PerFrameConstantBuffer),
            nvrhi::BindingSetItem::Texture_SRV(0, atlas->texture),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_InstanceBuffer),
            nvrhi::BindingSetItem::Sampler(0, m_FontManager.GetSampler())
        };
        m_BindingSet = m_Device->createBindingSet(bsd, m_BindingLayout);
    }

    return true;
}

void UiRenderPass::Render(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *frameBuffer,
    SimulationSnapshot &snapshot, double deltaTime) {

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

    if (m_PerFrameConstantBuffer) {
        const auto vp = frameBuffer->getFramebufferInfo().getViewport();
        const float w = vp.width();
        const float h = vp.height();

        // In the near future use the snapshot UICamera parameters to build the ortho matrix
        // snapshot.UICamera.position
        // snapshot.UICamera.dimensions
        // snapshot.UICamera.zoom

        // Map (0,0) top-left to (-1,+1), (w,h) to (+1,-1)
        // Matrix that does: x' = 2/w*x - 1, y' = 1 - 2/h*y
        glm::mat4 ortho(1.0f);
        ortho[0][0] = 2.0f / w;  ortho[1][0] = 0.0f;       ortho[2][0] = 0.0f; ortho[3][0] = -1.0f; // column 0..3
        ortho[0][1] = 0.0f;       ortho[1][1] = -2.0f / h; ortho[2][1] = 0.0f; ortho[3][1] =  1.0f;
        ortho[0][2] = 0.0f;       ortho[1][2] = 0.0f;       ortho[2][2] = 1.0f; ortho[3][2] =  0.0f;
        ortho[0][3] = 0.0f;       ortho[1][3] = 0.0f;       ortho[2][3] = 0.0f; ortho[3][3] =  1.0f;



        UIFrameCBData uiFrame{};
        uiFrame.Ortho = ortho;
        commandList->writeBuffer(m_PerFrameConstantBuffer, &uiFrame, sizeof(uiFrame));
    }

    nvrhi::GraphicsState state;
    state.pipeline = m_Pipeline;
    state.framebuffer = frameBuffer;
    state.viewport.addViewportAndScissorRect(frameBuffer->getFramebufferInfo().getViewport());
    state.vertexBuffers = { nvrhi::VertexBufferBinding(m_VertexBuffer, 0, 0) };
    state.indexBuffer = nvrhi::IndexBufferBinding(m_IndexBuffer, nvrhi::Format::R16_UINT, 0);

    if (snapshot.WorldSnapshotPtr) {

        size_t totalInstances = 0;
        size_t glyphCount = 0;
        UIInstanceCPU* glyphInstances = nullptr;

        const auto atlas = m_FontManager.GetAtlas(FontManager::DEFAULT_FONT);

        // TODO: Calculate total ammount of glyph instances required
        // .. malloc glyphInstances accordingly
        // .. fill glyphInstances in the loop below
        // .. upload all glyphInstances at once

        for (EntityId entity : snapshot.WorldSnapshotPtr->View<TransformComponent, TextComponent>()) {
            auto* transform = snapshot.WorldSnapshotPtr->GetComponent<TransformComponent>(entity);
            auto* text = snapshot.WorldSnapshotPtr->GetComponent<TextComponent>(entity);

            if (transform && text) {
                // Prepare instance data
                UIInstanceCPU instance{};
                instance.Transform = glm::mat4(1.0f);
                instance.Transform[0][0] = transform->Scale.x; // scaleX
                instance.Transform[1][1] = transform->Scale.y; // scaleY
                instance.Transform[3][0] = transform->Position.x; // posX
                instance.Transform[3][1] = transform->Position.y; // posY
                instance.Color = text->Color;
                instance.UVRect = glm::vec4(1.f, 1.f, 0.f, 0.f); // full texture
                instance.Flags = 1u << 0; // SAMPLE_TEXTURE

                //text->Text is an std::string

                glyphInstances[totalInstances++] = instance;
            }
        }

        if (totalInstances > 0) {
            commandList->writeBuffer(m_InstanceBuffer, glyphInstances, totalInstances * sizeof(UIInstanceCPU));
            state.bindings = { atlas->uiBindingSet ? atlas->uiBindingSet : m_BindingSet };
            commandList->setGraphicsState(state);
            nvrhi::DrawArguments uiDrawArgs;
            uiDrawArgs.vertexCount = m_IndexCount;
            uiDrawArgs.instanceCount = totalInstances;
            uiDrawArgs.startIndexLocation = 0;
            uiDrawArgs.startVertexLocation = 0;
            commandList->drawIndexed(uiDrawArgs);
        }
    }
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
