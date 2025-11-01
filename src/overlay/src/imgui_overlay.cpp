#include "imgui_overlay.h"

#include <d3dcompiler.h>
#include <unordered_map>
#include <nvrhi/nvrhi.h>

#include <WinString.h>
#include <fstream>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_dx12.h>

#include <renderer_dx11.h>
#include <renderer_dx12.h>

#include "imgui_internal.h"
#include "imgui_nvrhi.h"
#include "registered_font.h"

enum class RenderAPI : uint8_t { None, DirectX11, DirectX12 };


static RenderAPI g_RenderAPI = RenderAPI::None;
static ID3D12GraphicsCommandList* g_D3D12CommandList = nullptr;
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

    // Now we need cast the device_context to determine which backend to use
    /*
    const auto deviceCtxDX11 = static_cast<RendererBackendDX11*>(device_context);
    if (deviceCtxDX11 && deviceCtxDX11->m_Api == RendererBackendAPI::Direct3D11) {
        g_RenderAPI = RenderAPI::DirectX11;
        ImGui_ImplDX11_Init(deviceCtxDX11->m_Device11, deviceCtxDX11->m_ImmediateContext);
    }
    else {
        const auto deviceCtxDX12 = static_cast<RendererBackendDX12*>(device_context);
        if (deviceCtxDX12 && deviceCtxDX12->m_Api == RendererBackendAPI::Direct3D12) {
            g_RenderAPI = RenderAPI::DirectX12;

            // const auto descriptorHeap = deviceCtxDX12->m_Device->getNativeObject(nvrhi::ObjectTypes::D3D12_RenderTargetViewDescriptor);

            const auto d3d12_cmd_list = static_cast<ID3D12GraphicsCommandList *>(deviceCtxDX12->m_CommandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer);
            const auto test = static_cast<ID3D12GraphicsCommandList *>(deviceCtxDX12->m_Device->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer);
            g_D3D12CommandList = d3d12_cmd_list;

            ImGui_ImplDX12_InitInfo init_info = {};
            init_info.Device = deviceCtxDX12->m_Device12;
            init_info.CommandQueue = deviceCtxDX12->m_GraphicsQueue;
            init_info.NumFramesInFlight = deviceCtxDX12->m_Settings.maxFramesInFlight;
            init_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
            // Allocating SRV descriptors (for textures) is up to the application, so we provide callbacks.
            // (current version of the backend will only allocate one descriptor, future versions will need to allocate more)
            //init_info.SrvDescriptorHeap = static_cast<ID3D12DescriptorHeap*>(descriptorHeap.pointer);
            //init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle) { return g_pd3dSrvDescHeapAlloc.Alloc(out_cpu_handle, out_gpu_handle); };
            //init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle)            { return g_pd3dSrvDescHeapAlloc.Free(cpu_handle, gpu_handle); };
            ImGui_ImplDX12_Init(&init_info);
        }
    }
    */
    imgui_nvrhi = new ImGui_NVRHI();
    imgui_nvrhi->init(static_cast<nvrhi::IDevice*>(device_context));
    std::shared_ptr<RegisteredFont> m_FontOpenSans = CreateFontFromFile("assets/LiberationSans-Regular.ttf", 12.f);

}

EXPORT_FN void overlay_render(RenderData* renderData, nvrhi::IFramebuffer* framebuffer) {
    //if (g_RenderAPI == RenderAPI::DirectX11)
    //    ImGui_ImplDX11_NewFrame();
    //else if (g_RenderAPI == RenderAPI::DirectX12)
    //    ImGui_ImplDX12_NewFrame();
    //else return;

    //ImGui_ImplWin32_NewFrame();

    for (auto& font : m_fonts)
    {
        if (!font->GetScaledFont())
            font->CreateScaledFont(1.f);
    }

    imgui_nvrhi->updateFontTexture();

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(renderData->uiCamera.dimensions.x), static_cast<float>(renderData->uiCamera.dimensions.y));
    io.DeltaTime = 1 / 60.0f; // TODO: get actual delta time
    io.MouseDrawCursor = false;
    ImGui::NewFrame();

    ImGui::PushFont(m_fonts[0]->GetScaledFont(), 12.f);

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

    //if (g_RenderAPI == RenderAPI::DirectX11)
    //    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    //else if (g_RenderAPI == RenderAPI::DirectX12)
    //    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), nullptr);

    // Update and Render additional Platform Windows
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

EXPORT_FN void overlay_shutdown() {
    //if (g_RenderAPI == RenderAPI::DirectX11)
    //    ImGui_ImplDX11_Shutdown();
    //else if (g_RenderAPI == RenderAPI::DirectX12)
    //    ImGui_ImplDX12_Shutdown();
    //else return;
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