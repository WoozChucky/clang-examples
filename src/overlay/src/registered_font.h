#pragma once

#include <imgui.h>
#include "memory"

struct Blob {
    void* data;
    size_t size;
};

class RegisteredFont
{
public:
    void CreateScaledFont(float displayScale);
protected:
    friend class ImGui_Renderer;

    std::shared_ptr<Blob> m_data;
    bool const m_isDefault;
    bool const m_isCompressed;
    float const m_sizeAtDefaultScale;
    ImFont* m_imFont = nullptr;


    void ReleaseScaledFont();
public:
    // Creates an invalid font that will not add any ImGUI fonts
    RegisteredFont()
        : m_isDefault(false)
        , m_isCompressed(false)
        , m_sizeAtDefaultScale(0.f)
    { }

    // Creates a default font with the given size
    RegisteredFont(float size)
        : m_isDefault(true)
        , m_isCompressed(false)
        , m_sizeAtDefaultScale(size)
    { }

    // Creates a custom font
    RegisteredFont(std::shared_ptr<Blob> data, bool isCompressed, float size)
        : m_data(data)
        , m_isDefault(false)
        , m_isCompressed(isCompressed)
        , m_sizeAtDefaultScale(size)
    { }

    // Returns true if the custom font data has been successfully loaded.
    // This doesn't necessarily mean that the font data is valid: the actual font object is only created
    // in the first call to ImGui_Renderer::Animate(...). After that, use GetScaledFont()
    // to test if the font is valid.
    bool HasFontData() const { return m_data != nullptr; }

    // Returns the ImFont object that can be used with ImGUI.
    // Note that the returned pointer is transient and will change when screen DPI changes,
    // or when new fonts are loaded. Do not cache the returned value between frames.
    // The returned pointer may be NULL if the font has failed to load, which is OK for ImGUI's PushFont(...)
    ImFont* GetScaledFont() { return m_imFont; }
};