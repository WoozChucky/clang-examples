#include "renderer.h"

#include <chrono>
#include <thread>

#include "lib.h"

#include "renderer_dx12.h"

#include <nvrhi/nvrhi.h>


class DebugMessageCallback;

typedef struct RendererContext {
    nvrhi::DeviceHandle m_Device;
    nvrhi::CommandQueue m_CommandQueue;
    RendererBackend*    m_Backend;
    double              m_PreviousFrameTimestamp;
} RendererContext;

static RendererContext* g_RendererContext = nullptr;

void renderer_init(const int width, const int height, void* handle, BumpAllocator* persistentStorage) {

    g_RendererContext = reinterpret_cast<RendererContext *>(bump_alloc(persistentStorage, sizeof(RendererContext)));
    if (!g_RendererContext) {
        SM_ERROR("Failed to allocate RendererContext");
        return;
    }

    g_RendererContext->m_Backend = reinterpret_cast<RendererBackend *>(bump_alloc(persistentStorage, sizeof(RendererBackendDX12)));
    if (!g_RendererContext->m_Backend) {
        SM_ERROR("Failed to allocate RendererBackendDX12");
        return;
    }

    create_internal_instance(g_RendererContext->m_Backend);

    g_RendererContext->m_Device = create_device(g_RendererContext->m_Backend);
    if (!g_RendererContext->m_Device) {
        SM_ERROR("Failed to create NVRHI device");
        return;
    }

    create_swapchain(g_RendererContext->m_Backend, static_cast<HWND>(handle), width, height);
}

void renderer_shutdown() {

}

bool render(RenderData* renderData, pfnRenderUIOverlay uiOverlay) {

    if (!g_RendererContext || !g_RendererContext->m_Backend) {
        SM_ASSERT(false, "RendererContext or RendererBackend is null");
        return false;
    }

    std::chrono::high_resolution_clock::time_point currentTime = std::chrono::high_resolution_clock::now();
    double curTime = std::chrono::duration<double>(currentTime.time_since_epoch()).count();
    double elapsedTime = curTime - g_RendererContext->m_PreviousFrameTimestamp;

    uint32_t backendFrameIndex = *renderer_get_frame_index(g_RendererContext->m_Backend);

    if (backendFrameIndex > 0) {
        if (renderer_begin_frame(g_RendererContext->m_Backend)) {
            uint32_t frameIndex = backendFrameIndex;

            renderer_render(g_RendererContext->m_Backend);

            const bool presentSuccess = renderer_present(g_RendererContext->m_Backend);
            if (!presentSuccess) {
                return false;
            }
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(0));

    // TODO: Device runGC

    renderer_update_avg_frame_time(g_RendererContext->m_Backend, elapsedTime);
    g_RendererContext->m_PreviousFrameTimestamp = curTime;

    ++backendFrameIndex;
    return true;
}

void renderer_resize(int width, int height) {

}

void renderer_set_vsync(bool enabled) {

}

void* renderer_get_device() {
    return nullptr;
}

void* renderer_get_device_context() {
    return nullptr;
}