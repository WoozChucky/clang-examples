#pragma once
#include "lib.h"
#include "input.h"
#include "render.h"

#include <cstdint>
#include <WinString.h>

// define a callback to pass to overlay handling (THIS IS A TEMPORARY PLACEMENT)
typedef BOOL(*overlay_input_handler)(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

struct FrameStats {
    float deltaSeconds;    // Time for the last frame in seconds
    float frameTimeMs;     // Time for the last frame in milliseconds
    float fpsInstant;      // 1 / deltaSeconds
    float fpsSmoothed;     // Exponential moving average of FPS
    double elapsedSeconds; // Total accumulated time since start
    uint64_t frameCount;   // Number of frames since start
};

enum class PlatformType {
    PLATFORM_INVALID,
    PLATFORM_WINDOWS,
    PLATFORM_LINUX,
    PLATFORM_MAC,
    PLATFORM_COUNT
};

// Opaque handle for platform file watching
struct FileWatchHandle;

typedef struct PlatformContext {
    PlatformType m_Type;
    void* m_PlatformHandle;
    int m_Width;
    int m_Height;
    bool m_ResizeRequested;
    bool m_Running;
    bool m_Minimized;
    bool m_Focused;
    BumpAllocator* m_PersistentStorage;
    BumpAllocator* m_TransientStorage;
    KeyCodeID m_KeyCodeLookupTable[MAX_KEYCODES];
    float m_MusicVolume;
    overlay_input_handler m_OverlayInputHandler;
    bool m_DraggingWindow;
    FrameStats m_FrameStats; // Per-frame timing statistics
    Input* m_Input;
    RenderData* m_RenderData;
    SoundState* m_SoundState;
} PlatformContext;

PlatformContext* platform_init(BumpAllocator* persistentStorage, BumpAllocator* transientStorage);
void platform_shutdown(PlatformContext* ctx);
bool platform_create_window(PlatformContext* ctx, int width, int height, const char* title, void* windowProps);
void platform_fill_keycode_lookup_table(PlatformContext* ctx);
void platform_update_window(PlatformContext* ctx);
bool platform_is_minimized(PlatformContext* ctx);

// Dynamic library helpers
void* platform_load_dynamic_library(const char* dll);
void* platform_load_dynamic_function(void* dll, const char* funName);
bool platform_free_dynamic_library(void* dll);

// Audio
bool platform_init_audio(PlatformContext* ctx);
void platform_update_audio(PlatformContext* ctx, float dt);

// Misc
void platform_sleep(unsigned int ms);
// Deletes all files in the given directory (non-recursive). Returns true on success or if the directory doesn't exist.
bool platform_delete_all_files_in_directory(const char* directory);

// Generic file watching API (implemented per-platform)
// Starts watching the given file path. Returns a handle or nullptr on failure.
FileWatchHandle* platform_watch_file(const char* file_path, BumpAllocator* allocator);
// Returns true if the watched file has changed since the last call.
bool platform_file_changed(FileWatchHandle* handle);
// Stops watching and releases resources.
void platform_unwatch_file(FileWatchHandle* handle);

