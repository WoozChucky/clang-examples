#pragma once

#include <windows.h>
#include <string>

#include <Game.h>  // GameState + GameUpdateFunc/GameResizeFunc/GameExitFunc/GameGetVersionFunc + GAME_API_VERSION
#include "Systems.h"  // SystemScheduler + GameRegisterSystemsFunc
#include "Engine.h"

/**
 * @brief RAII wrapper around a dynamically-loaded Game.dll. Owns HMODULE,
 *        resolved symbol pointers, and the timestamped-copy filename actually
 *        loaded. Single-thread (GameThread) owner.
 */
class ENGINE_API GameLibrary {
public:
    GameLibrary() = default;
    ~GameLibrary();

    GameLibrary(const GameLibrary&) = delete;
    GameLibrary& operator=(const GameLibrary&) = delete;
    GameLibrary(GameLibrary&&) = delete;
    GameLibrary& operator=(GameLibrary&&) = delete;

    /**
     * @brief Loads `sourceDllPath` via a timestamped copy, resolves symbols,
     *        validates GAME_API_VERSION. On reload, invokes prior module's
     *        GameExit(state) before FreeLibrary, then swaps.
     * @return true on success; false if load/validation failed and the previous
     *         module (if any) is preserved.
     */
    bool LoadOrReload(const std::string& sourceDllPath, GameState* state);

    /** @brief Sets the scheduler GameLibrary clears before unload + repopulates
     *         after load via the game's GameRegisterSystems export. */
    void SetScheduler(SystemScheduler* scheduler) { m_Scheduler = scheduler; }

    /** @brief True when GameUpdate is installed. */
    bool IsValid() const { return m_pGameUpdate != nullptr; }

    /** @pre IsValid() must be true. */
    void Update(GameState* state) const { m_pGameUpdate(state); }

    /** @brief Invokes GameResize if exported; no-op otherwise. */
    void Resize(uint32_t width, uint32_t height) const {
        if (m_pGameResize) m_pGameResize(width, height);
    }

    /** @brief Invokes GameExit if exported, then frees current module. Idempotent. */
    void Unload(GameState* state);

private:
    HMODULE             m_Module          = nullptr;
    std::string         m_LoadedDllPath;
    GameUpdateFunc      m_pGameUpdate     = nullptr;
    GameResizeFunc      m_pGameResize     = nullptr;
    GameExitFunc        m_pGameExit       = nullptr;
    GameGetVersionFunc  m_pGameGetVersion = nullptr;
    GameRegisterSystemsFunc m_pGameRegisterSystems = nullptr;
    SystemScheduler*        m_Scheduler            = nullptr;  // not owned
    uint64_t            m_ReloadCounter   = 0;
};
