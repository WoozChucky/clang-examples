#include "font.h"

#include <freetype/freetype.h>
#include <nvrhi/utils.h>

void load_font(const char* filePath, int fontSize, FontAtlas& outAtlas, const nvrhi::DeviceHandle& device) {

    FT_Library ftLib = nullptr;
    if (FT_Init_FreeType(&ftLib)) {
        SM_ERROR("Could not init FreeType Library");
        return;
    }

    FT_Face fontFace = nullptr;
    if (FT_New_Face(ftLib, filePath, 0, &fontFace)) {
        SM_ERROR("Failed to load font: %s", filePath);
        FT_Done_FreeType(ftLib);
        return;
    }
    FT_Set_Pixel_Sizes(fontFace, 0, fontSize);

    const int texW = 512;
    const int texH = 512;
    const int padding = 2;
    int row = 0;
    int col = padding;

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
    if (!outAtlas.texture) return;

    uploadCL->beginTrackingTextureState(outAtlas.texture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    constexpr auto rowPitch = static_cast<size_t>(texW); // 1 byte per pixel
    uploadCL->writeTexture(outAtlas.texture, 0 /*array*/, 0 /*mip*/,
                           atlas.data(), rowPitch, 0 /*depthPitch*/);
    uploadCL->setPermanentTextureState(outAtlas.texture, nvrhi::ResourceStates::ShaderResource);
    uploadCL->commitBarriers();
    uploadCL->close();
    device->executeCommandList(uploadCL);

    // 3) Create sampler (clamp + linear)
    nvrhi::SamplerDesc sd;
    sd.setAllFilters(true) // linear
      .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    outAtlas.sampler = device->createSampler(sd);

    // 4) Create binding layout and set: t0 = font atlas, s0 = sampler
    nvrhi::BindingSetDesc bsd;
    bsd.bindings = {
        nvrhi::BindingSetItem::Texture_SRV(0, outAtlas.texture),  // t0
        nvrhi::BindingSetItem::Sampler(0, outAtlas.sampler)       // s0
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
        return;
    }

    // Shader note: sample `float alpha = tex.Sample(samp, uv).r;`
    SM_TRACE("Created font atlas binding set and layout");
}