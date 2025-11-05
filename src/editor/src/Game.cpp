#include "Game.h"

#include "ApplicationContext.h"

static GameState* g_GameState = nullptr;
static GameDebugBreakFn g_PlatformDebugBreak = nullptr;
static bool gKeysDown[KEY_LAST + 1] = {};

using namespace Input;

uint32_t GameGetVersion() {
    SM_TRACE("GameGetVersion");
    return 0;
}

void GameSetPlatformDebugBreak(const GameDebugBreakFn fn) {
    SM_TRACE("GameSetPlatformDebugBreak");
	if (fn) {
		g_PlatformDebugBreak = fn;
	}
}

void GameUpdate(GameState* state) {
    if (g_GameState != state) {
        g_GameState = state;
		SM_TRACE("Game: State Memory Updated")
	}

    if (!g_GameState) return;

	const auto inputRing = static_cast<SpscRing<InputEvent, ApplicationContext::InputRingSize>*>(g_GameState->PlatformInputHandle);
	const auto dt = static_cast<float>(state->DeltaTime);

	InputEvent ev{};
	while (inputRing->Pop(ev))
	{
		if (ev.Type == InputEventType::Key && ev.KeyEvent.Key == KEY_ESCAPE && ev.KeyEvent.Action == RELEASE)
		{
			SM_TRACE("GameUpdate: Shutdown requested via ESC key")
			g_GameState->QuitRequested = true;
			break;
		}

		const auto k = static_cast<int>(ev.KeyEvent.Key);
		if (k >= 0 && k <= KEY_LAST) {
			if (ev.KeyEvent.Action == PRESS || ev.KeyEvent.Action == REPEAT)   gKeysDown[k] = true;
			if (ev.KeyEvent.Action == RELEASE) gKeysDown[k] = false;
		}
	}

    {
    	const glm::vec3 rot= state->GameCamera.rotation; // pitch(x), yaw(y), roll(z)
    	const float cp = cosf(rot.x), sp = sinf(rot.x);
    	const float cy = cosf(rot.y), sy = sinf(rot.y);

    	// Camera-to-world forward for RH, y-up, default forward = -Z:
    	// forward = (Rx * Ry * Rz) * (0,0,-1) with roll=0
    	const glm::vec3 forward = glm::normalize(glm::vec3(
		  -sy,            // x
		   sp * cy,       // y
		  -cp * cy        // z
		));

    	// Right vector consistent with RH system
    	const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0,1,0)));

    	float moveSpeed = 7.5f; // units/sec

    	if (gKeysDown[KEY_W]) state->GameCamera.position += forward * (moveSpeed * dt);
    	if (gKeysDown[KEY_S]) state->GameCamera.position -= forward * (moveSpeed * dt);
    	if (gKeysDown[KEY_A]) state->GameCamera.position -= right   * (moveSpeed * dt);
    	if (gKeysDown[KEY_D]) state->GameCamera.position += right   * (moveSpeed * dt);

    	if (gKeysDown[KEY_SPACE]) { state->GameCamera.position.y += moveSpeed * dt; state->GameCamera.invalidate(); }
    	if (gKeysDown[KEY_LEFT_SHIFT]) { state->GameCamera.position.y -= moveSpeed * dt; state->GameCamera.invalidate(); }

    	constexpr float yawSpeed = glm::radians(120.0f);
    	auto& yaw = state->GameCamera.rotation.y;
		if (gKeysDown[KEY_Q]) yaw += yawSpeed * dt; state->GameCamera.invalidate();
		if (gKeysDown[KEY_E]) yaw -= yawSpeed * dt; state->GameCamera.invalidate();
    }

    switch (g_GameState->StateId)
    {
	    case GameStateId::Uninitialized:
			SM_TRACE("Initializing game...")
			g_GameState->StateId = GameStateId::MainMenu;
    		g_GameState->GameCamera.position = glm::vec3(0.0f, 5.0f, 10.0f);
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