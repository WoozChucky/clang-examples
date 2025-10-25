#include "lib.h"

#include "input.h"
#include "game.h"
#include "ui.h"
#include "sound.h"
#include "render.h"
#include "renderer.h"
#include "imgui_overlay.h"

#include "platform.h"
#ifdef _WIN32
#include "windows_platform.cpp"
const char* gameLibName = "game.dll";
const char* gameLoadLibName = "game_load.dll";
const char* overlayLibName = "imgui_overlay.dll";
const char* overlayLoadLibName = "imgui_overlay_load.dll";
#elif defined(__APPLE__)
#include "mac_platform.cpp"
const char* gameLibName = "game.dylib";
const char* gameLoadLibName = "game_load.dylib";
const char* overlayLibName = "imgui_overlay.dylib";
const char* overlayLoadLibName = "imgui_overlay_load.dylib";
#else
#include "linux_platform.cpp"
const char* gameLibName = "game.so";
const char* gameLoadLibName = "game_load.so";
const char* overlayLibName = "imgui_overlay.so";
const char* overlayLoadLibName = "imgui_overlay_load.so";
#endif

#include <chrono>

// This is the function pointer to update_game in game.cpp
typedef decltype(update_game) update_game_type;
static update_game_type* update_game_ptr;

typedef decltype(overlay_setup) setup_overlay_type;
static setup_overlay_type* setup_overlay_ptr;
typedef decltype(overlay_render) render_overlay_type;
static render_overlay_type* render_overlay_ptr;
typedef decltype(overlay_shutdown) shutdown_overlay_type;
static shutdown_overlay_type* shutdown_overlay_ptr;



void reload_game_dll(BumpAllocator* transientStorage);
void reload_ui_dll(BumpAllocator* transientStorage);
float get_delta_time();

static PlatformContext* g_PlatformContext = nullptr;


// int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
int main(int argc, char** argv) {

    BumpAllocator g_TransientStorage = make_bump_allocator(MB(50));
    BumpAllocator g_PersistentStorage = make_bump_allocator(MB(256));

    g_Input = reinterpret_cast<Input *>(bump_alloc(&g_PersistentStorage, sizeof(Input)));
    if(!g_Input)
    {
        SM_ERROR("Failed to allocate Input");
        return -1;
    }

    g_RenderData = reinterpret_cast<RenderData *>(bump_alloc(&g_PersistentStorage, sizeof(RenderData)));
    if(!g_RenderData)
    {
        SM_ERROR("Failed to allocate RenderData");
        return -1;
    }

    g_GameState = reinterpret_cast<GameState *>(bump_alloc(&g_PersistentStorage, sizeof(GameState)));
    if(!g_GameState)
    {
        SM_ERROR("Failed to allocate GameState");
        return -1;
    }

    g_UIState = reinterpret_cast<UIState *>(bump_alloc(&g_PersistentStorage, sizeof(UIState)));
    if(!g_UIState)
    {
        SM_ERROR("Failed to allocate UIState")
        return -1;
    }

    g_SoundState = reinterpret_cast<SoundState *>(bump_alloc(&g_PersistentStorage, sizeof(SoundState)));
    if(!g_SoundState)
    {
        SM_ERROR("Failed to allocate SoundState");
        return -1;
    }
    g_SoundState->transientStorage = &g_TransientStorage;
    g_SoundState->allocatedsoundsBuffer = bump_alloc(&g_PersistentStorage, SOUNDS_BUFFER_SIZE);
    if(!g_SoundState->allocatedsoundsBuffer)
    {
        SM_ERROR("Failed to allocated Sounds Buffer");
        return -1;
    }

    g_PlatformContext = platform_init(&g_PersistentStorage, &g_TransientStorage);
    if (!platform_create_window(g_PlatformContext, 800, 600, "My Window", nullptr)) {
        SM_ERROR("Failed to create window");
        return -1;
    }
    platform_fill_keycode_lookup_table(g_PlatformContext);
    if (!platform_init_audio(g_PlatformContext)) {
        SM_ERROR("Failed to initialize audio");
        return -1;
    }

    renderer_init(g_PlatformContext->m_Width, g_PlatformContext->m_Height, g_PlatformContext->m_PlatformHandle, &g_PersistentStorage);

    float displayFpsTimer = 0.0f;

    while (g_PlatformContext->m_Running) {
        const float dt = get_delta_time();

        {
            //TODO: Use file watcher instead of polling
            reload_game_dll(&g_TransientStorage);
            // reload_ui_dll(&g_TransientStorage);
        }

        displayFpsTimer += dt;
        if (displayFpsTimer >= 1.0f) {
            const FrameStats& stats = g_PlatformContext->m_FrameStats;
            SM_TRACE("FPS: %.2f (Smoothed: %.2f) Frame Time: %.2f ms", stats.fpsInstant, stats.fpsSmoothed, stats.frameTimeMs);
            displayFpsTimer = 0.0f;
        }

        platform_update_window(g_PlatformContext);

        if (g_PlatformContext->m_ResizeRequested) {
            g_PlatformContext->m_ResizeRequested = false;
            renderer_resize(g_PlatformContext->m_Width, g_PlatformContext->m_Height);
        }

        if (key_is_down(KEY_F5)) {
            renderer_set_vsync(false);
        }

        update_game(g_GameState, g_Input, g_RenderData, g_SoundState, g_UIState, &g_TransientStorage, dt);

        render(dt, g_RenderData, render_overlay_ptr);

        platform_update_audio(g_PlatformContext, dt);

        g_TransientStorage.used = 0; // Reset transient storage each frame
    }

    renderer_shutdown();

    platform_shutdown(g_PlatformContext);

    g_PlatformContext = nullptr;

    return 0;
}

void update_game(GameState* gameStateIn, 
                Input* inputIn,
                RenderData* renderDataIn, 
                SoundState* soundStateIn,
                UIState* uiStateIn,
                BumpAllocator* transientStorageIn,
                float dt) {
  update_game_ptr(gameStateIn, inputIn, renderDataIn, soundStateIn, uiStateIn, transientStorageIn, dt);
}

float get_delta_time() {
    using clock = std::chrono::high_resolution_clock;
    static auto last = clock::now();

    const auto currentTime = clock::now();
    const std::chrono::duration<double> delta = currentTime - last;
    last = currentTime;

    double dt = delta.count();
    if (dt < 0.0) dt = 0.0;               // guard against timer anomalies
    if (dt > 0.25) dt = 0.25;             // clamp to avoid huge spikes (e.g., during debugging)

    // Update per-frame stats so they are available for overlays/printing
    if (g_PlatformContext) {
        FrameStats& stats = g_PlatformContext->m_FrameStats;

        stats.deltaSeconds = static_cast<float>(dt);
        stats.frameTimeMs  = static_cast<float>(dt * 1000.0);
        stats.frameCount  += 1ULL;
        stats.elapsedSeconds += dt;

        const float fpsInstant = (dt > 0.0) ? static_cast<float>(1.0 / dt) : 0.0f;
        stats.fpsInstant = fpsInstant;

        // Exponential moving average for smoother FPS display
        if (stats.frameCount <= 1ULL) {
            stats.fpsSmoothed = fpsInstant;
        } else {
            const float alpha = 0.1f; // smoothing factor
            stats.fpsSmoothed = alpha * fpsInstant + (1.0f - alpha) * stats.fpsSmoothed;
        }
    }

    return static_cast<float>(dt);
}

void reload_game_dll(BumpAllocator* transientStorage)
{
    static void* gameDLL;
    static long long lastEditTimestampGameDLL;

    long long currentTimestampGameDLL = get_timestamp(gameLibName);
    if(currentTimestampGameDLL > lastEditTimestampGameDLL)
    {
        if(gameDLL)
        {
            bool freeResult = platform_free_dynamic_library(gameDLL);
            if (!freeResult)
            SM_ASSERT(freeResult, "Failed to free %s", gameLibName);
            gameDLL = nullptr;
            SM_TRACE("Freed %s", gameLibName);
        }

        while(!copy_file(gameLibName, gameLoadLibName, transientStorage))
        {
            platform_sleep(10);
        }
        SM_TRACE("Copied %s into %s", gameLibName, gameLoadLibName);

        gameDLL = platform_load_dynamic_library(gameLoadLibName);
        SM_ASSERT(gameDLL, "Failed to load %s", gameLoadLibName);

        update_game_ptr = (update_game_type*)platform_load_dynamic_function(gameDLL, "update_game");
        SM_ASSERT(update_game_ptr, "Failed to load update_game function");
        lastEditTimestampGameDLL = currentTimestampGameDLL;
    }
}

void reload_ui_dll(BumpAllocator* transientStorage)
{
    static void* uiDLL;
    static long long lastEditTimestampUIDLL;

    long long currentTimestampUIDLL = get_timestamp(overlayLibName);
    if(currentTimestampUIDLL > lastEditTimestampUIDLL)
    {
        if(uiDLL)
        {
            g_PlatformContext->m_OverlayInputHandler = nullptr;
            shutdown_overlay_ptr();
            bool freeResult = platform_free_dynamic_library(uiDLL);
            if (!freeResult)
            SM_ASSERT(freeResult, "Failed to free %s", overlayLibName);
            uiDLL = nullptr;
            SM_TRACE("Freed %s", overlayLibName);
        }

        while(!copy_file(overlayLibName, overlayLoadLibName, transientStorage))
        {
            platform_sleep(10);
        }
        SM_TRACE("Copied %s into %s", overlayLibName, overlayLoadLibName);

        uiDLL = platform_load_dynamic_library(overlayLoadLibName);
        SM_ASSERT(uiDLL, "Failed to load %s", overlayLoadLibName);

        setup_overlay_ptr = (setup_overlay_type*)platform_load_dynamic_function(uiDLL, "overlay_setup");
        SM_ASSERT(setup_overlay_ptr, "Failed to load overlay_setup function");

        shutdown_overlay_ptr = (shutdown_overlay_type*)platform_load_dynamic_function(uiDLL, "overlay_shutdown");
        SM_ASSERT(shutdown_overlay_ptr, "Failed to load overlay_shutdown function");

        render_overlay_ptr = (render_overlay_type*)platform_load_dynamic_function(uiDLL, "overlay_render");
        SM_ASSERT(render_overlay_ptr, "Failed to load overlay_render function");

        setup_overlay_ptr(g_PlatformContext->m_PlatformHandle,
                          nullptr,
                          renderer_get_device_context());

        g_PlatformContext->m_OverlayInputHandler = (overlay_input_handler)platform_load_dynamic_function(uiDLL, "overlay_handle_wndproc");
        SM_ASSERT(g_PlatformContext->m_OverlayInputHandler, "Failed to load overlay_handle_wndproc function");

        lastEditTimestampUIDLL = currentTimestampUIDLL;
    }
}
