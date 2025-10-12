#pragma once
#include "input.h"
#include "lib.h"


#include <WinString.h>
// define a callback to pass to overlay handling (THIS IS A TEMPORARY PLACEMENT)
typedef BOOL(*overlay_input_handler)(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

enum class PlatformType {
    PLATFORM_INVALID,
    PLATFORM_WINDOWS,
    PLATFORM_LINUX,
    PLATFORM_MAC,
    PLATFORM_COUNT
};

typedef struct PlatformContext {
    PlatformType m_Type;
    void* m_PlatformHandle;
    int m_Width;
    int m_Height;
    bool m_ResizeRequested;
    bool m_Running;
    BumpAllocator* m_PersistentStorage;
    BumpAllocator* m_TransientStorage;
    KeyCodeID m_KeyCodeLookupTable[MAX_KEYCODES];
    float m_MusicVolume;
    overlay_input_handler m_OverlayInputHandler;
    bool m_DraggingWindow;
} PlatformContext;

PlatformContext* platform_init(BumpAllocator* persistentStorage, BumpAllocator* transientStorage);
void platform_shutdown(PlatformContext* ctx);
bool platform_create_window(PlatformContext* ctx, int width, int height, const char* title, void* windowProps);
void platform_fill_keycode_lookup_table(PlatformContext* ctx);
void platform_update_window(PlatformContext* ctx);
void* platform_load_dynamic_library(const char* dll);
void* platform_load_dynamic_function(PlatformContext* ctx, void* dll, const char* funName);
bool platform_free_dynamic_library(PlatformContext* ctx, void* dll);
bool platform_init_audio(PlatformContext* ctx);
void platform_update_audio(PlatformContext* ctx, float dt);
void platform_sleep(unsigned int ms);
