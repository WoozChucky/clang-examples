#include "Game.h"

#include "ApplicationContext.h"

static GameState* g_GameState = nullptr;
static GameDebugBreakFn g_PlatformDebugBreak = nullptr;

uint32_t GameGetVersion() {
    SM_TRACE("GameGetVersion");
    return 0;
}

void GameSetPlatformDebugBreak(GameDebugBreakFn fn) {
    SM_TRACE("GameSetPlatformDebugBreak"); 
}

void GameUpdate(GameState* state) {
    if (g_GameState != state) {
        g_GameState = state;
		SM_TRACE("Game: State Update")
	}

    if (!g_GameState) return;

	const auto inputRing = static_cast<SpscRing<InputEvent, ApplicationContext::InputRingSize>*>(g_GameState->PlatformInputHandle);

	InputEvent ev{};
	while (inputRing->Pop(ev))
	{
		if (ev.TypeId == InputEvent::Key && ev.Button == 256 && ev.Action == 0)
		{
			SM_TRACE("GameUpdate: Shutdown requested via ESC key")
			g_GameState->QuitRequested = true;
			break;
		}
		// else ignore
	}

    switch (g_GameState->StateId)
    {
	    case GameStateId::Uninitialized:
			SM_TRACE("Initializing game...")
			g_GameState->StateId = GameStateId::MainMenu;
		    break;
	    case GameStateId::MainMenu:
		    break;
	    case GameStateId::InLevel:
		    break;
	    case GameStateId::InEditor:
		    break;
	    case GameStateId::Paused:

		    break;
	    default:
	        SM_ERROR("GameUpdate: Unknown GameStateId %u", static_cast<uint32_t>(g_GameState->StateId))
			break;
    }
}

void GameResize(uint32_t width, uint32_t height) {
    SM_TRACE("GameResize: %ux%u", width, height);
}

void GameExit() {
	/*
	if (g_GameState) {
        delete g_GameState;
        g_GameState = nullptr;
    }
	*/
	if (g_PlatformDebugBreak) {
		g_PlatformDebugBreak = nullptr;
	}
    SM_TRACE("GameExit");
}

// Assertion handler entry point used inside the game DLL.
// If the host EXE provided a platform-specific handler via GameSetPlatformDebugBreak,
// we forward to it; otherwise, we use a minimal fallback (log + debugbreak).
void platform_debug_break(const char* expr, const char* file, int line, const char* message)
{
	if (g_PlatformDebugBreak)
	{
		g_PlatformDebugBreak(expr, file, line, message);
		return;
	}

	// Fallback when no platform callback has been set yet
	_log("ASSERT:", "Expression: %s | File: %s | Line: %d | %s", TEXT_COLOR_RED,
		(expr ? expr : "<none>"), (file ? file : "<unknown>"), line, (message ? message : ""));
	DEBUG_BREAK();
}