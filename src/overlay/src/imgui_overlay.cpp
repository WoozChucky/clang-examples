#include "imgui_overlay.h"

#include <fstream>

#include <imgui.h>
#include <imgui_impl_win32.h>

#include "imgui_internal.h"
#include "imgui_nvrhi.h"
#include "registered_font.h"


static ImGui_NVRHI* imgui_nvrhi = nullptr;
static std::vector<std::shared_ptr<RegisteredFont>> m_fonts;

std::shared_ptr<RegisteredFont> CreateFontFromFile(const char* fontFile, float fontSize)
{
    auto fontData = std::make_shared<Blob>();

    std::ifstream fileStream(fontFile, std::ios::binary | std::ios::ate);
    if (!fileStream.is_open())
    {
        return nullptr;
    }

    void* fileBuffer = nullptr;
    std::streamsize fileSize = fileStream.tellg();
    fileStream.seekg(0, std::ios::beg);
    fileBuffer = malloc(static_cast<size_t>(fileSize));
    if (!fileStream.read(static_cast<char*>(fileBuffer), fileSize))
    {
        free(fileBuffer);
        return nullptr;
    }
    fontData->size = static_cast<size_t>(fileSize);
    fontData->data = fileBuffer;

    auto font = std::make_shared<RegisteredFont>(fontData, false, fontSize);
    m_fonts.push_back(font);

    return std::move(font);
}

EXPORT_FN void overlay_setup(void* platform_handle, void* device_context) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable GamePad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;       // Enable Multi-Viewport / Platform Windows
    io.ConfigViewportsNoAutoMerge = true;
    //io.ConfigViewportsNoTaskBarIcon = true;
    io.ConfigDockingAlwaysTabBar = true;
    io.ConfigDockingTransparentPayload = true;

    ImGui::StyleColorsDark();

    const float mainScale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT { 0, 0}, MONITOR_DEFAULTTOPRIMARY));

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(mainScale);
    style.FontScaleDpi = mainScale;
    io.ConfigDpiScaleFonts = true;          // [Experimental] Automatically overwrite style.FontScaleDpi in Begin() when Monitor DPI changes. This will scale fonts but _NOT_ scale sizes/padding for now.
    io.ConfigDpiScaleViewports = true;      // [Experimental] Scale Dear ImGui and Platform Windows when Monitor DPI changes.

    // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
        style.Colors[ImGuiCol_DockingEmptyBg].w = 0.0f;
    }

    ImGui_ImplWin32_Init(platform_handle);

    imgui_nvrhi = new ImGui_NVRHI();
    imgui_nvrhi->init(static_cast<nvrhi::IDevice*>(device_context));
    std::shared_ptr<RegisteredFont> m_FontOpenSans = CreateFontFromFile("assets/LiberationSans-Regular.ttf", 14.f);
}

EXPORT_FN void overlay_render(RenderData* renderData, nvrhi::IFramebuffer* framebuffer) {
    ImGui_ImplWin32_NewFrame();

    for (auto& font : m_fonts)
    {
        if (!font->GetScaledFont())
            font->CreateScaledFont(1.f);
    }

    imgui_nvrhi->updateFontTexture();

    ImGuiIO& io = ImGui::GetIO();
    ImGui::NewFrame();

    ImGui::PushFont(m_fonts[0]->GetScaledFont(), 14.f);

    // Ensure a DockSpace
    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

    // Build layout once
    static bool s_dockBuilt = false;
    if (!s_dockBuilt)
    {
        ImGuiID dockspace_id = ImGui::GetMainViewport()->ID; // Dock node for main viewport

        // Rebuild the dockspace to a known layout
        ImGui::DockBuilderRemoveNode(dockspace_id);                      // clear any previous layout
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);

        ImGuiID dock_id_right = 0;
        ImGuiID dock_id_main  = dockspace_id;

        // Split main dockspace: create a right dock at 28% width; remainder stays as main
        ImGui::DockBuilderSplitNode(dock_id_main, ImGuiDir_Right, 0.28f, &dock_id_right, &dock_id_main);

        // Dock your windows by exact title text
        ImGui::DockBuilderDockWindow("3D Engine Overlay", dock_id_right);
        ImGui::DockBuilderDockWindow("Hello, world!",      dock_id_main);

        ImGui::DockBuilderFinish(dockspace_id);
        s_dockBuilt = true;
    }

    const auto textElementCount = renderData->uiTexts.count;
    const auto uiRectsCount = renderData->uiRects.count;

    ImGui::Begin("Hello, world!");
    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
    ImGui::Text("Hello from the dynamically loaded UI Overlay!");
    ImGui::Separator();
    ImGui::Text("Text Elements: %d", textElementCount);
    ImGui::Separator();
    ImGui::Text("Rectangles: %d", uiRectsCount);
    ImGui::End();

    ImGui::Begin("3D Engine Overlay");
    static glm::vec3 Translation(0.0f);
    static glm::vec3 RotationDeg(0.0f);
    static glm::vec3 ScaleVec(1.0f);

    ImGui::SliderFloat3("Translation", &Translation.x, -10.0f, 10.0f);
    ImGui::SliderFloat3("Rotation (deg)", &RotationDeg.x, -180.0f, 180.0f);
    ImGui::SliderFloat3("Scale", &ScaleVec.x, 0.1f, 10.0f);

    // copy into your renderData (glm::mat4 assignment)
    renderData->modelPosition = Translation;
    renderData->modelRotation = RotationDeg;
    renderData->modelScale = ScaleVec;
    ImGui::End();

    ImGui::PopFont();

    ImGui::Render();

    imgui_nvrhi->render(framebuffer);

    // Update and Render additional Platform Windows
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

EXPORT_FN void overlay_shutdown() {
    if (imgui_nvrhi) {
        delete imgui_nvrhi;
        imgui_nvrhi = nullptr;
    }
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

EXPORT_FN BOOL overlay_handle_wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam) ? TRUE : FALSE;
}