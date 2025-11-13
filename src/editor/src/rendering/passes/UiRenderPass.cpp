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

    // Create per-font binding sets using the UI layout
    auto createFontUIBinding = [&](FontAtlas& fa)
    {
        if (!fa.texture || !fa.sampler) return;
        nvrhi::BindingSetDesc bsd;
        bsd.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_PerFrameConstantBuffer),        // b0
            nvrhi::BindingSetItem::Texture_SRV(0, fa.texture),                                         // t0
            nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_InstanceBuffer), // t1 SRV
            nvrhi::BindingSetItem::Sampler(0, fa.sampler)                                              // s0
        };
        fa.uiBindingSet = m_Device->createBindingSet(bsd, m_BindingLayout);
    };

    // Default font
    createFontUIBinding(m_FontAtlas);

    // Default UI binding set is font 0
    m_BindingSet = m_FontAtlas.uiBindingSet;

    return true;
}

void UiRenderPass::Render(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer,
    SimulationSnapshot &snapshot, double deltaTime) {
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
