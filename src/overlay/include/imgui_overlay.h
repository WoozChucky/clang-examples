#pragma once

#include <lib.h>
#include <render.h>
#include <Windows.h>

#include "nvrhi/nvrhi.h"

extern "C"
{
    EXPORT_FN void overlay_setup(void* platform_handle, void* device_context);
    EXPORT_FN void overlay_render(RenderData* renderData, nvrhi::IFramebuffer* framebuffer);
    EXPORT_FN void overlay_shutdown();
    EXPORT_FN BOOL overlay_handle_wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
}
