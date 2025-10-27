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
const char* libraryExtension = ".dll";
#elif defined(__APPLE__)
#include "mac_platform.cpp"
const char* libraryExtension = ".dylib";
#else
#include "linux_platform.cpp"
const char* libraryExtension = ".so";
#endif
const char* gameLibName = "game";
const char* overlayLibName = "imgui_overlay";
const char* gameLoadLibFormatName = "%s/%s_load_%lld%s"; // path, name, timestamp, extension
const char* overlayLoadFormatName = "%s/%s_load_%lld%s"; // path, name, timestamp, extension
const char* runtimeDirectory = "runtime";
static char* g_GameLibrary = nullptr;
static char* g_OverlayLibrary = nullptr;

#include <chrono>
#include <atomic>

// This is the function pointer to update_game in game.cpp
typedef decltype(game_update) game_update_type;
static game_update_type* game_update_ptr;
typedef decltype(game_resize) game_resize_type;
static game_resize_type* game_resize_ptr;

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

// File watching for DLL hot-reload
static FileWatchHandle* g_GameDllWatch = nullptr;
static std::atomic<bool> g_GameDllReloadRequested{false};
static FileWatchHandle* g_OverlayDllWatch = nullptr; // currently unused (overlay reload call is disabled)
static std::atomic<bool> g_OverlayDllReloadRequested{false};

template<typename T0>
void visit(T0& variantData) {
    using T = std::decay_t<T0>;
    if constexpr (std::is_same_v<T, UIPanel>) {
        SM_TRACE("Visiting UIPanel");
    } else if constexpr (std::is_same_v<T, UIButton>) {
        SM_TRACE("Visiting UIButton");
    } else {
        SM_TRACE("Visiting unknown UI element");
    }
}

// int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
int main(int argc, char** argv) {

    BumpAllocator g_TransientStorage = make_bump_allocator(MB(50));
    BumpAllocator g_PersistentStorage = make_bump_allocator(MB(256));

    UIElement rootNode;
    UIElement panelNode;
    panelNode.type = UIElementType::PANEL;
    panelNode.parent = &rootNode;
    panelNode.data.emplace<UIPanel>();

    rootNode.children.push_back(&panelNode);

    panelNode.VisitData([]<typename T0>(T0& data) {
        visit(data);
    });

    const auto g_Input = reinterpret_cast<Input *>(bump_alloc(&g_PersistentStorage, sizeof(Input)));
    if(!g_Input)
    {
        SM_ERROR("Failed to allocate Input");
        return -1;
    }

    const auto g_RenderData = reinterpret_cast<RenderData *>(bump_alloc(&g_PersistentStorage, sizeof(RenderData)));
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
    if (!g_PlatformContext) {
        SM_ERROR("Failed to initialize platform");
        return -1;
    }
    g_PlatformContext->m_Input = g_Input;
    g_PlatformContext->m_RenderData = g_RenderData;
    if (!platform_create_window(g_PlatformContext, 1920, 1080, "My Window", nullptr)) {
        SM_ERROR("Failed to create window");
        return -1;
    }
    platform_fill_keycode_lookup_table(g_PlatformContext);
    if (!platform_init_audio(g_PlatformContext)) {
        SM_ERROR("Failed to initialize audio");
        return -1;
    }

    // Start watching the game DLL for hot-reload
    size_t fullPathSize = strlen(gameLibName) + strlen(libraryExtension) + 1;
    g_GameLibrary = bump_alloc(&g_PersistentStorage, fullPathSize);
    snprintf(g_GameLibrary, fullPathSize, "%s%s", gameLibName, libraryExtension);
    g_GameDllWatch = platform_watch_file(g_GameLibrary, &g_PersistentStorage);
    if (!g_GameDllWatch) {
        SM_WARN("File watcher couldn't be created for %s; hot-reload will be disabled.", gameLibName);
    }
    // Force initial load of the DLL at startup
    g_GameDllReloadRequested.store(true);

    fullPathSize = strlen(overlayLibName) + strlen(libraryExtension) + 1;
    g_OverlayLibrary = bump_alloc(&g_PersistentStorage, fullPathSize);
    snprintf(g_OverlayLibrary, fullPathSize, "%s%s", overlayLibName, libraryExtension);
    //g_OverlayDllWatch = platform_watch_file(g_OverlayLibrary, &g_PersistentStorage);
    //if (!g_OverlayDllWatch) {
    //    SM_WARN("File watcher couldn't be created for %s; hot-reload will be disabled.", overlayLibName);
    //}

    renderer_init(g_PlatformContext->m_Width, g_PlatformContext->m_Height, g_PlatformContext->m_PlatformHandle, &g_PersistentStorage);
    SM_TRACE("[Renderer] Persistent storage allocated: %d MB", g_PersistentStorage.used / (1024 * 1024));

    size_t frameAllocationBytes = 0;

    while (g_PlatformContext->m_Running && !g_GameState->quitRequested) {
        const float dt = get_delta_time();

        {
            // Drive hot-reload via platform file watcher
            if (g_GameDllWatch && platform_file_changed(g_GameDllWatch)) {
                g_GameDllReloadRequested.store(true);
            }
            reload_game_dll(&g_TransientStorage);
            // For overlay DLL:
            // if (g_OverlayDllWatch && platform_file_changed(g_OverlayDllWatch)) { g_OverlayDllReloadRequested.store(true); }
            // reload_ui_dll(&g_TransientStorage);
        }

        g_GameState->fps = g_PlatformContext->m_FrameStats.fpsInstant;
        g_GameState->frameTime = g_PlatformContext->m_FrameStats.frameTimeMs;

        platform_update_window(g_PlatformContext);

        if (g_PlatformContext->m_ResizeRequested) {
            g_PlatformContext->m_ResizeRequested = false;
            game_resize(g_PlatformContext->m_Width, g_PlatformContext->m_Height);
            renderer_resize(g_PlatformContext->m_Width, g_PlatformContext->m_Height);
        }

        if (key_released_this_frame(g_Input, KEY_F5)) {
            renderer_toggle_vsync();
        }

        game_update(g_GameState, g_Input, g_RenderData, g_SoundState, g_UIState, &g_TransientStorage, &g_PersistentStorage, frameAllocationBytes, dt);

        // Skip rendering when the window is minimized to avoid unnecessary work
        if (!platform_is_minimized(g_PlatformContext))
        {
            render(dt, g_RenderData, &g_TransientStorage, render_overlay_ptr);
        }
        else
        {
            platform_sleep(16);
        }

        platform_update_audio(g_PlatformContext, dt);

        frameAllocationBytes = g_TransientStorage.used;
        g_TransientStorage.used = 0; // Reset transient storage each frame
    }

    renderer_shutdown();

    // Stop file watchers
    if (g_GameDllWatch) {
        platform_unwatch_file(g_GameDllWatch);
        g_GameDllWatch = nullptr;
    }
    if (g_OverlayDllWatch) {
        platform_unwatch_file(g_OverlayDllWatch);
        g_OverlayDllWatch = nullptr;
    }

    platform_shutdown(g_PlatformContext);

    g_PlatformContext = nullptr;

    return 0;
}

void game_update(GameState* gameStateIn,
                Input* inputIn,
                RenderData* renderDataIn, 
                SoundState* soundStateIn,
                UIState* uiStateIn,
                BumpAllocator* transientStorageIn,
                BumpAllocator* persistentStorageIn,
                size_t lastFrameAllocationBytes,
                float dt) {
    if (game_update_ptr) game_update_ptr(gameStateIn, inputIn, renderDataIn, soundStateIn, uiStateIn, transientStorageIn, persistentStorageIn, lastFrameAllocationBytes, dt);
}

void game_resize(const int width, const int height) {
    if (game_resize_ptr) game_resize_ptr(width, height);
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

const char * generate_timestamped_filename(const char* fmt, const char* folder, const char* libName, const char* extension, BumpAllocator* transientStorage)
{
    // Get current timestamp
    std::chrono::time_point<std::chrono::system_clock> ts = std::chrono::system_clock::now();
    const long long timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(ts.time_since_epoch()).count();

    const size_t neededSize = strlen(folder) + strlen(libName) + strlen(extension)+  32; // extra space for timestamp and null terminator
    char* tempPath = bump_alloc(transientStorage, neededSize);
    if (!tempPath)
        return nullptr;

    snprintf(tempPath, neededSize, fmt, folder, libName, timestamp, extension);
    return tempPath;
}

void reload_game_dll(BumpAllocator* transientStorage)
{
    // Debounced hot-reload driven by platform file watcher
    static void* gameDLL = nullptr;
    static bool pending = false;
    static std::chrono::steady_clock::time_point pendingSince{};
    static constexpr auto debounceWindow = std::chrono::milliseconds(250);

    // Consume new change requests
    if (g_GameDllReloadRequested.exchange(false))
    {
        pending = true;
        pendingSince = std::chrono::steady_clock::now();
    }

    if (!pending)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    // For the very first load (no DLL loaded yet), skip debounce to avoid starting with a blank frame
    if (gameDLL != nullptr && now - pendingSince < debounceWindow)
    {
        return; // Still within debounce window
    }

    const auto newLibName = generate_timestamped_filename(gameLoadLibFormatName, runtimeDirectory, gameLibName, libraryExtension, transientStorage);
    if (!newLibName)
    {
        SM_ERROR("Failed to generate timestamped filename for %s%s", g_GameLibrary, libraryExtension);
        return;
    }

    //TODO(Nuno): The problem with this approach is that the file is locked because we did not unload the previous DLL yet.
    // for now we use a generated filename with timestamp to avoid this problem.

    // 1) Copy new DLL under a load-safe name; if it fails (e.g., file locked or not yet created), try again next frame
    if(!copy_file(g_GameLibrary, newLibName, transientStorage))
    {
        return;
    }
    SM_TRACE("Copied %s into %s", g_GameLibrary, newLibName);

    // 2) Load new DLL into a temporary handle first (don’t drop current until we validate)
    void* newDLL = platform_load_dynamic_library(newLibName);
    if (!newDLL)
    {
        SM_ERROR("Failed to load %s", newLibName);
        return;
    }

    // 3) Resolve and validate API version before swapping
    using game_get_api_version_type = uint32_t (*)();
    const auto get_api_version = (game_get_api_version_type)platform_load_dynamic_function(newDLL, "game_get_api_version");
    if (!get_api_version)
    {
        SM_ERROR("%s missing game_get_api_version(); keeping previous game DLL.", newLibName);
        platform_free_dynamic_library(newDLL);
        return;
    }
    const uint32_t dllVersion = get_api_version();
    if (dllVersion != GAME_API_VERSION)
    {
        SM_ERROR("Game DLL API version mismatch. EXE=%u, DLL=%u. Keeping previous game DLL.", GAME_API_VERSION, dllVersion);
        platform_free_dynamic_library(newDLL);
        return;
    }

    // 4) Resolve required entry points on the new DLL
    auto new_update = (game_update_type*)platform_load_dynamic_function(newDLL, "game_update");
    auto new_resize = (game_resize_type*)platform_load_dynamic_function(newDLL, "game_resize");
    if (!new_update || !new_resize)
    {
        SM_ERROR("Failed to resolve required game DLL symbols (update/resize). Keeping previous game DLL.");
        platform_free_dynamic_library(newDLL);
        return;
    }

    // 4.1) Optionally provide platform-specific debug break callback to the game DLL
    using game_set_platform_debug_break_type = void (*)(game_debug_break_fn);
    if (auto set_dbg = (game_set_platform_debug_break_type)platform_load_dynamic_function(newDLL, "game_set_platform_debug_break"))
    {
        // Pass the EXE's platform_debug_break so the DLL can use MessageBox, etc.
        set_dbg(&platform_debug_break);
    }
    else
    {
        SM_WARN("Game DLL missing set_platform_debug_break (optional). Using DLL fallback for asserts.");
    }

    // 5) Swap: free previous, install new pointers+handle
    if (gameDLL)
    {
        bool freeResult = platform_free_dynamic_library(gameDLL);
        if (!freeResult)
            SM_ASSERT(freeResult, "Failed to free %s", gameLibName);
        SM_TRACE("Freed previous %s", gameLibName);
    }
    gameDLL = newDLL;
    game_update_ptr = new_update;
    game_resize_ptr = new_resize;

    pending = false; // consumed
}

void reload_ui_dll(BumpAllocator* transientStorage)
{
    // Debounced hot-reload for the UI overlay DLL
    static void* uiDLL = nullptr;
    static long long lastLoadedTimestamp = 0;
    static long long pendingTimestamp = 0;
    static std::chrono::steady_clock::time_point pendingSince;
    static const auto debounceWindow = std::chrono::milliseconds(250);

    const long long currentTimestamp = get_timestamp(overlayLibName);
    if (currentTimestamp <= 0)
        return;

    if (currentTimestamp != lastLoadedTimestamp)
    {
        if (pendingTimestamp != currentTimestamp)
        {
            pendingTimestamp = currentTimestamp;
            pendingSince = std::chrono::steady_clock::now();
            return; // wait for stability
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - pendingSince < debounceWindow)
        {
            return; // still debouncing
        }

        // Ready to reload
        if (uiDLL)
        {
            g_PlatformContext->m_OverlayInputHandler = nullptr;
            if (shutdown_overlay_ptr)
                shutdown_overlay_ptr();
            bool freeResult = platform_free_dynamic_library(uiDLL);
            if (!freeResult)
                SM_ASSERT(freeResult, "Failed to free %s", overlayLibName);
            uiDLL = nullptr;
            SM_TRACE("Freed %s", overlayLibName);
        }

        const auto newLibName = generate_timestamped_filename(overlayLoadFormatName, runtimeDirectory, overlayLibName, libraryExtension, transientStorage);
        if (!newLibName)
        {
            SM_ERROR("Failed to generate timestamped filename for %s", gameLibName);
            return;
        }

        while(!copy_file(g_OverlayLibrary, newLibName, transientStorage))
        {
            platform_sleep(10);
        }
        SM_TRACE("Copied %s into %s", g_OverlayLibrary, newLibName);

        uiDLL = platform_load_dynamic_library(newLibName);
        SM_ASSERT(uiDLL, "Failed to load %s", newLibName);

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

        lastLoadedTimestamp = currentTimestamp;
        pendingTimestamp = 0;
    }
}
