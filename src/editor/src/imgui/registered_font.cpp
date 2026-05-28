#include "registered_font.h"

void RegisteredFont::CreateScaledFont(float displayScale)
{
    ImFontConfig fontConfig;
    fontConfig.SizePixels = m_sizeAtDefaultScale * displayScale;

    m_imFont = nullptr;

    if (m_data)
    {
        fontConfig.FontDataOwnedByAtlas = false;
        if (m_isCompressed)
        {
            m_imFont = ImGui::GetIO().Fonts->AddFontFromMemoryCompressedTTF(
                (void*)(m_data->data), (int)(m_data->size), 0.f, &fontConfig);
        }
        else
        {
            m_imFont = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
                (void*)(m_data->data), (int)(m_data->size), 0.f, &fontConfig);
        }
    }
    else if (m_isDefault)
    {
        m_imFont = ImGui::GetIO().Fonts->AddFontDefault(&fontConfig);
    }

    if (m_imFont)
    {
        ImGui::GetIO().Fonts->TexRef = ImTextureRef();
    }
}

void RegisteredFont::ReleaseScaledFont()
{
    m_imFont = nullptr;
}
