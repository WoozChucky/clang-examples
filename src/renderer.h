#pragma once
#include "render.h"
#include <nvrhi/nvrhi.h>

struct BumpAllocator;
struct RenderData;

// declare PFN to UI Overlay render function
typedef void(*pfnRenderUIOverlay)();

void renderer_init(int width, int height, void* handle, BumpAllocator* persistentStorage);
void renderer_shutdown();
bool render(float dt, RenderData* renderData, BumpAllocator* transientStorage, pfnRenderUIOverlay uiOverlay = nullptr);
void renderer_resize(int width, int height);
void renderer_toggle_vsync();

void* renderer_get_device();
void* renderer_get_device_context();

struct FontAtlas
{
    uint32_t width = 512, height = 512, pixelHeight = 0;
    Glyph glyphs[128] = {};
    nvrhi::TextureHandle texture;          // grayscale atlas (R8_UNORM)
    nvrhi::SamplerHandle sampler;          // clamp + linear
    nvrhi::BindingLayoutHandle layout;     // layout: t0 + s0
    nvrhi::BindingSetHandle bindingSet;    // binds font texture at t0, sampler at s0
};

// HLSL for a textured cube using a texture atlas
inline auto G_VS_HLSL = R"(
// Per-frame CB (register b0)
cbuffer PerFrame : register(b0)
{
    float4x4 uModel;   // model transform (optional)
    float4x4 uVP;      // view-proj
    float3   uCameraPos;
    float    pad0;
    float3   uSunColor; // optional
    float    uAmbient;
};

// Per-draw CB (register b1)
cbuffer PerDraw : register(b1)
{
    float3 gChunkOffset;      // world offset for sub-chunk (if used)
    float  pad1;
    float2 gTileSizeUV;       // (tileW / atlasW, tileH / atlasH)
    float2 gTileTexelOffset;  // small offset to avoid bleeding (e.g., 0.5/atlasW)
    float4 gMaterialTint;     // optional per-draw tint
};

// Input vertex (matches D3D11 input layout)
struct VSIn
{
    float3 Pos            : POSITION;   // float3 world/local pos (for now)
    uint   MaterialFlags  : TEXCOORD0;  // at offset 12
    uint   TilePacked     : TEXCOORD1;  // at offset 16
    uint   PackedLightUV  : TEXCOORD2;  // at offset 20: [bits 0..15]=packedLight, [16..31]=uvFace
};

// Vertex -> Pixel struct
struct VSOut
{
    float4 PosH   : SV_POSITION;
    float2 UV     : TEXCOORD0;
    float3 World  : TEXCOORD1;
    float  Light  : TEXCOORD2;
    float  AO     : TEXCOORD3;
    uint   MatID  : TEXCOORD4;
};

// decode baked light stored as: [bits 0..3]=blockLight, [4..7]=skyLight
inline float DecodeLight(uint packed)
{
    uint blockLight = packed & 0xFu;
    uint skyLight   = (packed >> 4) & 0xFu;
    float blk = (float)blockLight / 15.0f;
    float sky = (float)skyLight / 15.0f;
    return saturate(sky + blk * 0.9f);
}

// decode AO stored in bits [8..11] (0..15)
inline float DecodeAO(uint packed)
{
    uint ao = (packed >> 8) & 0xFu;
    return 1.0f - (float)ao / 15.0f;
}

VSOut main_vs(VSIn vin)
{
    VSOut o;

    // world position (optionally apply chunk offset)
    float3 worldPos = vin.Pos + gChunkOffset;

    //float4 wpos = float4(worldPos, 1.0f);
    float4 wpos = mul(float4(vin.Pos + gChunkOffset, 1.0f), uModel);
    o.PosH = mul(uVP, wpos);

    // decode tile index (we stored 16-bit tile value in low 16 bits)
    uint tilePacked = vin.TilePacked & 0xFFFFu;
    uint tileX = tilePacked & 0xFFu;
    uint tileY = (tilePacked >> 8) & 0xFFu;

    // unpack packedLight and uvFace
    uint packedLight = vin.PackedLightUV & 0xFFFFu;        // bits 0..15
    uint uvFace      = (vin.PackedLightUV >> 16) & 0xFFFFu; // bits 16..31

    uint uBit = uvFace & 1u;
    uint vBit = (uvFace >> 1) & 1u;
    // uint faceIndex = (uvFace >> 8) & 0xFFu; // if you encoded faceIndex there

    float2 localUV = float2((float)uBit, (float)vBit);

    // tile origin / uv compute (same as before)
    float2 tileOrigin = float2((float)tileX, (float)tileY) * gTileSizeUV;
    float2 uv = tileOrigin + localUV * gTileSizeUV;
    uv += gTileTexelOffset;

    o.UV = uv;
    o.World = worldPos;
    o.Light = DecodeLight(packedLight);
    o.AO = DecodeAO(packedLight);
    o.MatID = vin.MaterialFlags & 0xFFFFu;

    return o;
}
)";

inline auto G_PS_HLSL = R"(
Texture2D gAtlas : register(t0);       // atlas texture (bind to slot t0)
SamplerState gSampler : register(s0);  // sampler (bind to slot s0)

// Match the VSOut structure for input semantics
struct PSIn
{
    float4 PosH   : SV_POSITION;
    float2 UV     : TEXCOORD0;
    float3 World  : TEXCOORD1;
    float  Light  : TEXCOORD2;
    float  AO     : TEXCOORD3;
    uint   MatID  : TEXCOORD4;
};

// Per-frame and Per-draw CBs are expected to be bound the same as VS (b0, b1)
// If you need uAmbient or tint here, they come from the same CBs.

cbuffer PerFrame : register(b0)
{
    float4x4 uModel;
    float4x4 uVP;
    float3   uCameraPos;
    float    pad0;
    float3   uSunColor;
    float    uAmbient;
};

cbuffer PerDraw : register(b1)
{
    float3 gChunkOffset;
    float  pad1;
    float2 gTileSizeUV;
    float2 gTileTexelOffset;
    float4 gMaterialTint;
};

// Pixel shader
float4 main_ps(PSIn pin) : SV_Target
{
    float4 albedo = gAtlas.Sample(gSampler, pin.UV);

    // If this material is cutout, perform alpha-test here (enable per-material)
    // if (albedo.a < 0.5f) discard;

    // Combine baked light and ambient, then apply AO and tint
    float3 lit = albedo.rgb * (uAmbient + pin.Light * (1.0f - uAmbient));
    lit *= pin.AO;
    lit *= gMaterialTint.rgb;

    return float4(lit, albedo.a);
}
)";

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