#include "SmaaRenderPass.h"

#include "Renderer.h"
#include <nvrhi/utils.h>
#include <glm/vec4.hpp>
#include <string>

#include "lib.h"             // SM_ERROR
#include "smaa/SMAA_hlsl.h"  // SMAA_HLSL_SOURCE (editable copy of submodule SMAA.hlsl, MIT)
#include "AreaTex.h"         // areaTexBytes, AREATEX_WIDTH/HEIGHT/PITCH (submodule Textures/)
#include "SearchTex.h"       // searchTexBytes, SEARCHTEX_WIDTH/HEIGHT/PITCH (submodule Textures/)

struct SmaaFrameCB { glm::vec4 RtMetrics; };
static_assert(sizeof(SmaaFrameCB) % 16 == 0, "SmaaFrameCB must be 16-byte aligned");

namespace {
// Config prelude defined BEFORE the SMAA.hlsl body. We use SMAA_CUSTOM_SL (not SMAA_HLSL_4):
// SMAA.hlsl's built-in HLSL4 path declares samplers with legacy Effects state-block syntax
// (`SamplerState X { Filter=...; }`) that DXC/SM6 rejects. SMAA_CUSTOM_SL lets us supply the
// device macros + register-based samplers ourselves. Slots are globally unique (NVRHI flat
// Vulkan binding): CB b0, textures t1.., samplers s4/s5. The preset is on the first line.
static const char* SMAA_PRELUDE = R"(
#define SMAA_PRESET_HIGH 1          // <-- change to SMAA_PRESET_ULTRA / _LOW etc. + recompile
#define SMAA_RT_METRICS uRtMetrics
#define SMAA_CUSTOM_SL 1
cbuffer SmaaCB : register(b0) { float4 uRtMetrics; };
SamplerState LinearSampler : register(s4);
SamplerState PointSampler  : register(s5);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
)";

static const char* EDGE_VS = R"(
struct VSOut { float4 PosH:SV_POSITION; float2 tc:TEXCOORD0; float4 off[3]:TEXCOORD1; };
VSOut main_vs(uint vid : SV_VertexID){
    VSOut o; o.tc = float2((vid<<1)&2, vid&2);
    o.PosH = float4(o.tc*float2(2,-2)+float2(-1,1),0,1);
    SMAAEdgeDetectionVS(o.tc, o.off);
    return o;
}
)";
static const char* EDGE_PS = R"(
Texture2D uColor : register(t1);
struct PSIn { float4 PosH:SV_POSITION; float2 tc:TEXCOORD0; float4 off[3]:TEXCOORD1; };
float4 main_ps(PSIn i):SV_Target { return float4(SMAALumaEdgeDetectionPS(i.tc, i.off, uColor), 0.0, 0.0); }
)";

static const char* WEIGHT_VS = R"(
struct VSOut { float4 PosH:SV_POSITION; float2 tc:TEXCOORD0; float2 pix:TEXCOORD1; float4 off[3]:TEXCOORD2; };
VSOut main_vs(uint vid : SV_VertexID){
    VSOut o; o.tc = float2((vid<<1)&2, vid&2);
    o.PosH = float4(o.tc*float2(2,-2)+float2(-1,1),0,1);
    SMAABlendingWeightCalculationVS(o.tc, o.pix, o.off);
    return o;
}
)";
static const char* WEIGHT_PS = R"(
Texture2D uEdges : register(t1); Texture2D uArea : register(t2); Texture2D uSearch : register(t3);
struct PSIn { float4 PosH:SV_POSITION; float2 tc:TEXCOORD0; float2 pix:TEXCOORD1; float4 off[3]:TEXCOORD2; };
float4 main_ps(PSIn i):SV_Target {
    return SMAABlendingWeightCalculationPS(i.tc, i.pix, i.off, uEdges, uArea, uSearch, float4(0,0,0,0));
}
)";

static const char* BLEND_VS = R"(
struct VSOut { float4 PosH:SV_POSITION; float2 tc:TEXCOORD0; float4 off:TEXCOORD1; };
VSOut main_vs(uint vid : SV_VertexID){
    VSOut o; o.tc = float2((vid<<1)&2, vid&2);
    o.PosH = float4(o.tc*float2(2,-2)+float2(-1,1),0,1);
    SMAANeighborhoodBlendingVS(o.tc, o.off);
    return o;
}
)";
static const char* BLEND_PS = R"(
Texture2D uColor : register(t1); Texture2D uBlend : register(t2);
struct PSIn { float4 PosH:SV_POSITION; float2 tc:TEXCOORD0; float4 off:TEXCOORD1; };
float4 main_ps(PSIn i):SV_Target { return SMAANeighborhoodBlendingPS(i.tc, i.off, uColor, uBlend); }
)";

static std::string Compose(const char* entry) {
    std::string s; s.reserve(96 * 1024);
    s += SMAA_PRELUDE; s += SMAA_HLSL_SOURCE; s += entry; return s;
}
} // namespace

bool SmaaRenderPass::Initialize(nvrhi::IDevice* device, Renderer* renderer)
{
    m_Device = device; m_Renderer = renderer;
    if (!m_Device || !m_Renderer) return false;

    auto mk = [&](nvrhi::ShaderType t, const char* stage, const char* entry, const char* prof) {
        const std::string src = Compose(stage);
        return m_Renderer->CreateShader(t, src.c_str(), src.size(), entry, prof);
    };
    m_EdgeVS   = mk(nvrhi::ShaderType::Vertex, EDGE_VS,   "main_vs", "vs_6_1");
    m_EdgePS   = mk(nvrhi::ShaderType::Pixel,  EDGE_PS,   "main_ps", "ps_6_1");
    m_WeightVS = mk(nvrhi::ShaderType::Vertex, WEIGHT_VS, "main_vs", "vs_6_1");
    m_WeightPS = mk(nvrhi::ShaderType::Pixel,  WEIGHT_PS, "main_ps", "ps_6_1");
    m_BlendVS  = mk(nvrhi::ShaderType::Vertex, BLEND_VS,  "main_vs", "vs_6_1");
    m_BlendPS  = mk(nvrhi::ShaderType::Pixel,  BLEND_PS,  "main_ps", "ps_6_1");
    if (!m_EdgeVS || !m_EdgePS || !m_WeightVS || !m_WeightPS || !m_BlendVS || !m_BlendPS) {
        SM_ERROR("SmaaRenderPass: shader compilation failed");
        return false;
    }

    { nvrhi::SamplerDesc sd; sd.setAllFilters(true);  sd.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp); m_LinearClamp = m_Device->createSampler(sd); }
    { nvrhi::SamplerDesc sd; sd.setAllFilters(false); sd.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp); m_PointClamp  = m_Device->createSampler(sd); }

    auto upload = [&](const unsigned char* bytes, uint32_t w, uint32_t h, nvrhi::Format fmt, uint32_t rowPitch, const char* name) -> nvrhi::TextureHandle {
        nvrhi::TextureDesc td; td.width = w; td.height = h; td.format = fmt;
        td.dimension = nvrhi::TextureDimension::Texture2D; td.debugName = name;
        td.isShaderResource = true;
        nvrhi::TextureHandle tex = m_Device->createTexture(td);
        if (!tex) return tex;
        nvrhi::CommandListHandle cl = m_Device->createCommandList();
        cl->open();
        cl->beginTrackingTextureState(tex, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
        cl->writeTexture(tex, 0, 0, bytes, rowPitch);
        cl->setPermanentTextureState(tex, nvrhi::ResourceStates::ShaderResource);
        cl->commitBarriers();
        cl->close();
        m_Device->executeCommandList(cl);
        return tex;
    };
    m_AreaTex   = upload(areaTexBytes,   AREATEX_WIDTH,   AREATEX_HEIGHT,   nvrhi::Format::RG8_UNORM, AREATEX_PITCH,   "SMAA AreaTex");
    m_SearchTex = upload(searchTexBytes, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, nvrhi::Format::R8_UNORM,  SEARCHTEX_PITCH, "SMAA SearchTex");
    if (!m_AreaTex || !m_SearchTex) { SM_ERROR("SmaaRenderPass: lookup texture creation failed"); return false; }

    auto makeLayout = [&](std::vector<nvrhi::BindingLayoutItem> items) {
        nvrhi::BindingLayoutDesc d; d.visibility = nvrhi::ShaderType::All; d.bindings = std::move(items);
        if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
            d.setBindingOffsets(nvrhi::VulkanBindingOffsets{}.setConstantBufferOffset(0).setShaderResourceOffset(0).setSamplerOffset(0));
        return m_Device->createBindingLayout(d);
    };
    m_EdgeLayout   = makeLayout({ nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Sampler(4), nvrhi::BindingLayoutItem::Sampler(5) });
    m_WeightLayout = makeLayout({ nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Texture_SRV(2), nvrhi::BindingLayoutItem::Texture_SRV(3), nvrhi::BindingLayoutItem::Sampler(4), nvrhi::BindingLayoutItem::Sampler(5) });
    m_BlendLayout  = makeLayout({ nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Texture_SRV(2), nvrhi::BindingLayoutItem::Sampler(4), nvrhi::BindingLayoutItem::Sampler(5) });
    if (!m_EdgeLayout || !m_WeightLayout || !m_BlendLayout) { SM_ERROR("SmaaRenderPass: binding layout creation failed"); return false; }

    m_FrameCB = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(SmaaFrameCB), "SmaaRenderPass FrameCB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer).setKeepInitialState(true));
    return m_FrameCB != nullptr;
}

bool SmaaRenderPass::EnsureTargets(uint32_t width, uint32_t height)
{
    if (m_EdgesTex && m_Width == width && m_Height == height) return true;
    m_Width = width; m_Height = height;

    auto rt = [&](nvrhi::Format fmt, const char* name) {
        nvrhi::TextureDesc td; td.width = width; td.height = height; td.format = fmt;
        td.dimension = nvrhi::TextureDimension::Texture2D; td.isRenderTarget = true;
        td.isShaderResource = true;
        td.debugName = name; td.initialState = nvrhi::ResourceStates::ShaderResource; td.keepInitialState = true;
        td.clearValue = nvrhi::Color(0.f); td.useClearValue = true;
        return m_Device->createTexture(td);
    };
    m_EdgesTex = rt(nvrhi::Format::RG8_UNORM, "SMAA edges");
    m_BlendTex = rt(nvrhi::Format::RGBA8_UNORM, "SMAA blend");
    if (!m_EdgesTex || !m_BlendTex) { SM_ERROR("SmaaRenderPass: intermediate target alloc failed"); return false; }
    m_EdgesFb = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_EdgesTex));
    m_BlendFb = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_BlendTex));
    return m_EdgesFb && m_BlendFb;
}

void SmaaRenderPass::Shutdown()
{
    m_EdgePipeline = m_WeightPipeline = m_BlendPipeline = nullptr;
    m_EdgeLayout = m_WeightLayout = m_BlendLayout = nullptr;
    m_EdgesFb = m_BlendFb = nullptr; m_EdgesTex = m_BlendTex = nullptr;
    m_AreaTex = m_SearchTex = nullptr; m_FrameCB = nullptr;
    m_LinearClamp = m_PointClamp = nullptr;
    m_EdgeVS = m_EdgePS = m_WeightVS = m_WeightPS = m_BlendVS = m_BlendPS = nullptr;
    m_Device = nullptr; m_Renderer = nullptr; m_Width = m_Height = 0;
}

void SmaaRenderPass::OnResize(uint32_t /*width*/, uint32_t /*height*/)
{
    m_EdgePipeline = m_WeightPipeline = m_BlendPipeline = nullptr;
    m_EdgesFb = m_BlendFb = nullptr; m_EdgesTex = m_BlendTex = nullptr;
    m_Width = m_Height = 0;
}

void SmaaRenderPass::Render(nvrhi::ICommandList* commandList,
                            nvrhi::IFramebuffer* frameBuffer,
                            SimulationSnapshot& /*snapshot*/,
                            const ECS* /*world*/,
                            double /*deltaTime*/,
                            FrameAllocator* /*frameAllocator*/)
{
    nvrhi::ITexture* scene = m_Renderer->GetSceneColorTexture();
    if (!scene) return;

    const auto fbi = frameBuffer->getFramebufferInfo();
    if (!EnsureTargets(fbi.width, fbi.height)) return;

    // Lazy pipelines (rebuilt on resize). Each sub-pass: no depth, no cull, no blend.
    auto makePipe = [&](nvrhi::IShader* vs, nvrhi::IShader* ps, nvrhi::IBindingLayout* layout, nvrhi::IFramebuffer* fb) {
        nvrhi::GraphicsPipelineDesc pso;
        pso.VS = vs; pso.PS = ps; pso.bindingLayouts = { layout };
        pso.primType = nvrhi::PrimitiveType::TriangleList;
        pso.renderState.depthStencilState.depthTestEnable = false;
        pso.renderState.depthStencilState.depthWriteEnable = false;
        pso.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        nvrhi::BlendState::RenderTarget rt; rt.setBlendEnable(false).setColorWriteMask(nvrhi::ColorMask::All);
        pso.renderState.blendState.setRenderTarget(0, rt);
        return m_Device->createGraphicsPipeline(pso, fb->getFramebufferInfo());
    };
    if (!m_EdgePipeline)   m_EdgePipeline   = makePipe(m_EdgeVS,   m_EdgePS,   m_EdgeLayout,   m_EdgesFb);
    if (!m_WeightPipeline) m_WeightPipeline = makePipe(m_WeightVS, m_WeightPS, m_WeightLayout, m_BlendFb);
    if (!m_BlendPipeline)  m_BlendPipeline  = makePipe(m_BlendVS,  m_BlendPS,  m_BlendLayout,  frameBuffer);

    commandList->beginMarker("SmaaRenderPass");

    SmaaFrameCB cb{};
    cb.RtMetrics = glm::vec4(1.0f / fbi.width, 1.0f / fbi.height, (float)fbi.width, (float)fbi.height);
    commandList->writeBuffer(m_FrameCB, &cb, sizeof(cb));

    auto draw = [&](nvrhi::IGraphicsPipeline* pipe, nvrhi::IFramebuffer* fb, nvrhi::BindingSetHandle bs) {
        nvrhi::GraphicsState st; st.pipeline = pipe; st.framebuffer = fb; st.bindings = { bs };
        st.viewport.addViewportAndScissorRect(fb->getFramebufferInfo().getViewport());
        commandList->setGraphicsState(st);
        nvrhi::DrawArguments a; a.vertexCount = 3; commandList->draw(a);
    };

    // 1) Edge detection: scene -> edges. Clear edges first (SMAA expects fresh edges).
    commandList->clearTextureFloat(m_EdgesTex, nvrhi::AllSubresources, nvrhi::Color(0.f));
    {
        nvrhi::BindingSetDesc d; d.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameCB),
            nvrhi::BindingSetItem::Texture_SRV(1, scene),
            nvrhi::BindingSetItem::Sampler(4, m_LinearClamp),
            nvrhi::BindingSetItem::Sampler(5, m_PointClamp) };
        draw(m_EdgePipeline, m_EdgesFb, m_Device->createBindingSet(d, m_EdgeLayout));
    }
    // 2) Blend-weight calc: edges + area + search -> blend. Clear blend first.
    commandList->clearTextureFloat(m_BlendTex, nvrhi::AllSubresources, nvrhi::Color(0.f));
    {
        nvrhi::BindingSetDesc d; d.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameCB),
            nvrhi::BindingSetItem::Texture_SRV(1, m_EdgesTex),
            nvrhi::BindingSetItem::Texture_SRV(2, m_AreaTex),
            nvrhi::BindingSetItem::Texture_SRV(3, m_SearchTex),
            nvrhi::BindingSetItem::Sampler(4, m_LinearClamp),
            nvrhi::BindingSetItem::Sampler(5, m_PointClamp) };
        draw(m_WeightPipeline, m_BlendFb, m_Device->createBindingSet(d, m_WeightLayout));
    }
    // 3) Neighborhood blend: scene + blend -> swapchain.
    {
        nvrhi::BindingSetDesc d; d.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameCB),
            nvrhi::BindingSetItem::Texture_SRV(1, scene),
            nvrhi::BindingSetItem::Texture_SRV(2, m_BlendTex),
            nvrhi::BindingSetItem::Sampler(4, m_LinearClamp),
            nvrhi::BindingSetItem::Sampler(5, m_PointClamp) };
        draw(m_BlendPipeline, frameBuffer, m_Device->createBindingSet(d, m_BlendLayout));
    }

    commandList->endMarker();
}
