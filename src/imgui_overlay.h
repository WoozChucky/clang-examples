#pragma once

#include "lib.h"
#include <Windows.h>

extern "C"
{
    EXPORT_FN void overlay_setup(void* platform_handle, void* device, void* device_context);
    EXPORT_FN void overlay_render();
    EXPORT_FN void overlay_shutdown();
    EXPORT_FN BOOL overlay_handle_wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

}
