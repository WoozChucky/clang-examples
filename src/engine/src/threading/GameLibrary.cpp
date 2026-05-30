#include "GameLibrary.h"

#include <chrono>
#include <filesystem>
#include <system_error>

#include "lib.h"  // SM_TRACE / SM_WARN / SM_ERROR

namespace {

std::string MakeTimestampedCopyPath(const std::string& srcDir,
                                    const std::string& baseName,
                                    uint64_t counter)
{
    const auto ts = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return srcDir + "/" + baseName + "_load_" +
           std::to_string(ts) + "_" + std::to_string(counter) + ".dll";
}

} // namespace

GameLibrary::~GameLibrary() {
    // Cannot call GameExit here — no GameState pointer at destruction.
    // Caller (GameThread shutdown) must call Unload(state) before destruction.
    if (m_Module) {
        // Safety net: if Unload() wasn't called, still destroy game.dll-owned
        // ISystem instances while their vtables are mapped, before FreeLibrary.
        if (m_Scheduler) m_Scheduler->Clear();
        FreeLibrary(m_Module);
        m_Module = nullptr;
    }
}

bool GameLibrary::LoadOrReload(const std::string& sourceDllPath, GameState* state) {
    namespace fs = std::filesystem;

    const fs::path srcPath(sourceDllPath);
    const auto srcDir   = srcPath.parent_path().string().empty()
                            ? std::string(".")
                            : srcPath.parent_path().string();
    const auto baseName = srcPath.stem().string();
    const auto copyPath = MakeTimestampedCopyPath(srcDir, baseName, ++m_ReloadCounter);

    std::error_code ec;
    fs::copy_file(srcPath, copyPath, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        SM_WARN("GameLibrary: copy_file('%s' -> '%s') failed: %s",
                sourceDllPath.c_str(), copyPath.c_str(), ec.message().c_str());
        return false;
    }

    HMODULE newModule = LoadLibraryA(copyPath.c_str());
    if (!newModule) {
        SM_ERROR("GameLibrary: LoadLibraryA('%s') failed (GetLastError=%lu)",
                 copyPath.c_str(), GetLastError());
        fs::remove(copyPath, ec);
        return false;
    }

    auto pVersion = reinterpret_cast<GameGetVersionFunc>(
        GetProcAddress(newModule, "GameGetVersion"));
    auto pUpdate  = reinterpret_cast<GameUpdateFunc>(
        GetProcAddress(newModule, "GameUpdate"));
    if (!pVersion || !pUpdate) {
        SM_ERROR("GameLibrary: missing required exports in '%s' (GameGetVersion=%p, GameUpdate=%p)",
                 copyPath.c_str(), (void*)pVersion, (void*)pUpdate);
        FreeLibrary(newModule);
        fs::remove(copyPath, ec);
        return false;
    }

    const uint32_t v = pVersion();
    if (v != GAME_API_VERSION) {
        SM_ERROR("GameLibrary: API version mismatch (editor=%u, dll=%u). Rebuild both targets.",
                 GAME_API_VERSION, v);
        FreeLibrary(newModule);
        fs::remove(copyPath, ec);
        return false;
    }

    auto pResize = reinterpret_cast<GameResizeFunc>(GetProcAddress(newModule, "GameResize"));
    auto pExit   = reinterpret_cast<GameExitFunc>(GetProcAddress(newModule, "GameExit"));
    auto pRegisterSystems = reinterpret_cast<GameRegisterSystemsFunc>(
        GetProcAddress(newModule, "GameRegisterSystems"));

    if (m_Module) {
        if (m_pGameExit) m_pGameExit(state);
        // Destroy game.dll-owned ISystem instances while their vtables are still
        // mapped (this module is about to be FreeLibrary'd). See spec hot-reload section.
        if (m_Scheduler) m_Scheduler->Clear();
        FreeLibrary(m_Module);
        fs::remove(m_LoadedDllPath, ec);  // best-effort
        SM_TRACE("GameLibrary: unloaded previous module '%s'", m_LoadedDllPath.c_str());
    }

    m_Module          = newModule;
    m_LoadedDllPath   = copyPath;
    m_pGameUpdate     = pUpdate;
    m_pGameResize     = pResize;
    m_pGameExit       = pExit;
    m_pGameGetVersion = pVersion;
    m_pGameRegisterSystems = pRegisterSystems;

    SM_TRACE("GameLibrary: loaded '%s' (API v%u)", copyPath.c_str(), v);

    // Forward Engine.dll's installed log sink into the freshly-loaded Game.dll so its logs reach
    // the editor console (and survive hot-reload). Null in runtime/server → game logs to stdout only.
    if (auto pInstallLog = reinterpret_cast<void(*)(LogSinkFn)>(
            GetProcAddress(newModule, "GameInstallLogSink"))) {
        pInstallLog(g_SmLogSink.load(std::memory_order_relaxed));
    }

    if (m_Scheduler && m_pGameRegisterSystems) {
        m_pGameRegisterSystems(m_Scheduler);
        SM_TRACE("GameLibrary: registered %zu system(s)", m_Scheduler->Count());
    }
    return true;
}

void GameLibrary::Unload(GameState* state) {
    if (!m_Module) return;
    if (m_pGameExit) m_pGameExit(state);
    if (m_Scheduler) m_Scheduler->Clear();   // before FreeLibrary — vtables still mapped
    FreeLibrary(m_Module);
    std::error_code ec;
    std::filesystem::remove(m_LoadedDllPath, ec);
    m_Module = nullptr;
    m_pGameUpdate     = nullptr;
    m_pGameResize     = nullptr;
    m_pGameExit       = nullptr;
    m_pGameGetVersion = nullptr;
    m_pGameRegisterSystems = nullptr;
}
