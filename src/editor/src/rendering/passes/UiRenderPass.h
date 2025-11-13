#pragma once

#include "IRenderPass.h"
#include <nvrhi/nvrhi.h>

#include <freetype/freetype.h>
#include <unordered_map>

#include "nvrhi/utils.h"

struct Glyph
{
    glm::vec2 size;          // pixel size in the atlas
    glm::vec2 offset;        // bearing from baseline (top-left convention)
    glm::vec2 advance;       // advance in pixels
    glm::vec2 textureCoords; // top-left in atlas (pixels)
};

struct FontAtlas
{
    uint32_t width = 512, height = 512, pixelHeight = 0;
    Glyph glyphs[128] = {};
    nvrhi::TextureHandle texture;          // grayscale atlas (R8_UNORM)
    nvrhi::BindingLayoutHandle layout;     // layout: t0 + s0 (created in font.cpp, not used by UI)
    nvrhi::BindingSetHandle bindingSet;    // binds font texture at t0, sampler at s0 (font.cpp)
    // UI-specific binding set (uses UI pass layout: b0 (per-frame), t0 (font), t1 (instances), s0)
    nvrhi::BindingSetHandle uiBindingSet;
};

class FontManager final {
public:
    struct FontKey {
        std::string Path;
        size_t Size;
        bool operator==(const FontKey& rhs) const {
            return Path == rhs.Path && Size == rhs.Size;
        }
    };

    struct FontKeyHash {
        size_t operator()(const FontKey& key) const {
            const size_t h1 = std::hash<std::string>{}(key.Path);
            const size_t h2 = std::hash<size_t>{}(key.Size);
            return h1 ^ (h2 << 1); // better distribution
        }
    };

    static const FontKey DEFAULT_FONT;

    bool LoadAtlas(const char* filePath, const size_t fontSize, nvrhi::IDevice* device) {
        FT_Library ftLib = nullptr;
        if (FT_Init_FreeType(&ftLib)) {
            SM_ERROR("Could not init FreeType Library");
            return false;
        }

        FT_Face fontFace = nullptr;
        if (FT_New_Face(ftLib, filePath, 0, &fontFace)) {
            SM_ERROR("Failed to load font: %s", filePath);
            FT_Done_FreeType(ftLib);
            return false;
        }
        FT_Set_Pixel_Sizes(fontFace, 0, fontSize);

        constexpr int texW = 512;
        constexpr int texH = 512;
        constexpr int padding = 2;
        int row = 0;
        int col = padding;

        FontAtlas outAtlas{};
        std::vector<uint8_t> atlas(texW * texH, 0);

        for (FT_ULong charIndex = 32; charIndex < 127; ++charIndex) {

            const FT_UInt glyphIndex = FT_Get_Char_Index(fontFace, charIndex);
            if (FT_Load_Glyph(fontFace, glyphIndex, FT_LOAD_DEFAULT)) continue;
            if (FT_Render_Glyph(fontFace->glyph, FT_RENDER_MODE_NORMAL)) continue;

            const auto& bm = fontFace->glyph->bitmap;
            const int gw = static_cast<int>(bm.width);
            const int gh = static_cast<int>(bm.rows);

            if (col + fontFace->glyph->bitmap.width + padding >= texW)
            {
                col = padding;
                row += fontSize;
                if (row + fontSize >= texH) {
                    SM_WARN("Font atlas is full, some glyphs were not packed");
                    break; // no more space
                }
            }

            for (unsigned int y = 0; y < fontFace->glyph->bitmap.rows; ++y)
            {
                for (unsigned int x = 0; x < fontFace->glyph->bitmap.width; ++x)
                {
                    atlas[(row + y) * texW + col + x] =
                        fontFace->glyph->bitmap.buffer[y * fontFace->glyph->bitmap.width + x];
                }
            }

            Glyph g{};
            g.textureCoords = glm::vec2(static_cast<float>(col), static_cast<float>(row));
            g.size          = glm::vec2(static_cast<float>(gw), static_cast<float>(gh));
            g.advance       = glm::vec2(static_cast<float>(fontFace->glyph->advance.x >> 6),
                                        static_cast<float>(fontFace->glyph->advance.y >> 6));
            g.offset        = glm::vec2(static_cast<float>(fontFace->glyph->bitmap_left),
                                        static_cast<float>(fontFace->glyph->bitmap_top));

            outAtlas.glyphs[charIndex] = g;

            col += gw + padding;
        }

        FT_Done_Face(fontFace);
        FT_Done_FreeType(ftLib);

        outAtlas.width = texW;
        outAtlas.height = texH;
        outAtlas.pixelHeight = fontSize;

        // 2) Create NVRHI texture (R8_UNORM) and upload atlas
        auto uploadCL = device->createCommandList();
        uploadCL->open();

        nvrhi::TextureDesc td;
        td.debugName = "FontAtlas";
        td.width = texW;
        td.height = texH;
        td.depth = 1;
        td.arraySize = 1;
        td.mipLevels = 1;
        td.dimension = nvrhi::TextureDimension::Texture2D;
        td.isRenderTarget = false;
        td.isUAV = false;
        td.format = nvrhi::Format::R8_UNORM;   // grayscale
        outAtlas.texture = device->createTexture(td);
        if (!outAtlas.texture) return false;

        uploadCL->beginTrackingTextureState(outAtlas.texture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
        constexpr auto rowPitch = static_cast<size_t>(texW); // 1 byte per pixel
        uploadCL->writeTexture(outAtlas.texture, 0 /*array*/, 0 /*mip*/,
                               atlas.data(), rowPitch, 0 /*depthPitch*/);
        uploadCL->setPermanentTextureState(outAtlas.texture, nvrhi::ResourceStates::ShaderResource);
        uploadCL->commitBarriers();
        uploadCL->close();
        device->executeCommandList(uploadCL);

        if (!m_Sampler) {
            // 3) Create sampler (clamp + linear)
            nvrhi::SamplerDesc sd;
            sd.setAllFilters(true) // linear
              .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
            m_Sampler = device->createSampler(sd);
        }

        // 4) Create binding layout and set: t0 = font atlas, s0 = sampler
        nvrhi::BindingSetDesc bsd;
        bsd.bindings = {
            nvrhi::BindingSetItem::Texture_SRV(0, outAtlas.texture),  // t0
            nvrhi::BindingSetItem::Sampler(0, m_Sampler)       // s0
        };

        if (!nvrhi::utils::CreateBindingSetAndLayout(
                device,
                nvrhi::ShaderType::All,
                0,
                bsd,
                outAtlas.layout,
                outAtlas.bindingSet))
        {
            SM_ERROR("Failed to create font atlas binding set and layout");
            return false;
        }

        m_AtlasMap.insert(std::make_pair(FontKey {.Path = filePath, .Size = fontSize}, std::move(outAtlas)));

        SM_TRACE("Created font atlas binding set and layout");
        return true;
    }

    nvrhi::SamplerHandle GetSampler() const {
        return m_Sampler;
    }

    FontAtlas* GetAtlas(const FontKey &fontKey) {
        const auto it = m_AtlasMap.find(fontKey);
        return it != m_AtlasMap.end() ? &it->second : nullptr;
    }

private:
    nvrhi::SamplerHandle    m_Sampler; // clamp + linear
    std::unordered_map<FontKey, FontAtlas, FontKeyHash> m_AtlasMap;
};

class UiRenderPass final : public IRenderPass {
public:
    UiRenderPass() = default;
    ~UiRenderPass() override = default;

    bool Initialize(nvrhi::IDevice* device, Renderer* renderer) override;
    void Render(
        nvrhi::ICommandList* commandList,
        nvrhi::IFramebuffer* frameBuffer,
        SimulationSnapshot& snapshot,
        double deltaTime) override;
    void Shutdown() override;
    void OnResize(uint32_t width, uint32_t height) override;

private:
    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;

    // Resources
    nvrhi::BufferHandle m_VertexBuffer;             // static quad vertices (pos, uv)
    nvrhi::BufferHandle m_IndexBuffer;              // static quad indices
    nvrhi::BufferHandle m_PerFrameConstantBuffer;
    nvrhi::BufferHandle m_InstanceBuffer;
    nvrhi::ShaderHandle m_VS;
    nvrhi::ShaderHandle m_PS;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BindingSetHandle m_BindingSet;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    uint32_t m_IndexCount = 0;
    uint32_t m_MaxInstances = 16384;

    FontManager m_FontManager{};
};