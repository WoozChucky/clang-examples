#pragma once

struct BumpAllocator;
struct RenderData;

// declare PFN to UI Overlay render function
typedef void(*pfnRenderUIOverlay)();

void renderer_init(int width, int height, void* handle, BumpAllocator* persistentStorage);
void renderer_shutdown();
void render(RenderData* renderData, pfnRenderUIOverlay uiOverlay = nullptr);
void renderer_resize(int width, int height);
void renderer_set_vsync(bool enabled);

void* renderer_get_device();
void* renderer_get_device_context();
