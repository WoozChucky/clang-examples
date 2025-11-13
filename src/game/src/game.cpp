#include "Game.h"

#include <ApplicationContext.h>
#include <Input.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

static GameState* g_GameState = nullptr;
static GameDebugBreakFn g_PlatformDebugBreak = nullptr;
static bool gKeysDown[KEY_LAST + 1] = {};
static bool gKeysPressedThisFrame[KEY_LAST + 1] = {}; // NEW: Track single-frame presses
static int32_t gMouseWheel = 0;
static bool   g_MouseAimEnabled = false;        // toggled by T
static double g_MouseX = 0.0, g_MouseY = 0.0;   // last mouse position in window coords

using namespace Input;

void DrainInput(SpscRing<InputEvent, ApplicationContext::InputRingSize>* inputRing);
void HandleFreeLook(GameState* state);
void HandleCameraMovement(GameState* state);

// Helper function to check if key was pressed this frame (single press, not held)
inline bool IsKeyPressedThisFrame(int key) {
    if (key < 0 || key > KEY_LAST) return false;
    return gKeysPressedThisFrame[key];
}

// Helper function to check if key is currently held down
inline bool IsKeyDown(int key) {
    if (key < 0 || key > KEY_LAST) return false;
    return gKeysDown[key];
}

uint32_t GameGetVersion() {
    SM_TRACE("[GAMEDLL] GameGetVersion");
    return 0;
}

void GameSetPlatformDebugBreak(const GameDebugBreakFn fn) {
    SM_TRACE("[GAMEDLL] GameSetPlatformDebugBreak");
	if (fn) {
		g_PlatformDebugBreak = fn;
	}
}

void GameUpdate(GameState* state) {
    if (g_GameState != state) {
        g_GameState = state;
		SM_TRACE("[GAMEDLL] State Memory Updated")
	}

    if (!g_GameState) return;

	// Clear pressed-this-frame flags at start of each update
	memset(gKeysPressedThisFrame, 0, sizeof(gKeysPressedThisFrame));

	const auto inputRing = static_cast<SpscRing<InputEvent, ApplicationContext::InputRingSize>*>(g_GameState->PlatformInputHandle);

	DrainInput(inputRing);

	HandleCameraMovement(g_GameState);

    {
	    // Use IsKeyPressedThisFrame to add only ONE entity per key press (not per frame while held)
		if (IsKeyPressedThisFrame(KEY_1)) {
			const auto entityId = g_GameState->World.CreateEntity();
			SM_TRACE("Added new Entity (%llu)", entityId)
		}
    }

    switch (g_GameState->StateId)
    {
	    case GameStateId::Uninitialized:
			SM_TRACE("[GAMEDLL] Initializing game...")
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
	        SM_ERROR("[GAMEDLL] GameUpdate: Unknown GameStateId %u", static_cast<uint32_t>(g_GameState->StateId))
			break;
    }
}

void HandleCameraMovement(GameState* state)
{
	const auto dt = static_cast<float>(state->DeltaTime);

	const glm::vec3 rot = state->GameCamera.rotation; // pitch(x), yaw(y), roll(z)
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
	const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));

	float moveSpeed = 7.5f; // units/sec

	// Movement - use IsKeyDown for continuous movement
	if (IsKeyDown(KEY_W)) state->GameCamera.position += forward * (moveSpeed * dt);
	if (IsKeyDown(KEY_S)) state->GameCamera.position -= forward * (moveSpeed * dt);
	if (IsKeyDown(KEY_A)) state->GameCamera.position -= right * (moveSpeed * dt);
	if (IsKeyDown(KEY_D)) state->GameCamera.position += right * (moveSpeed * dt);

	// Elevation
	if (IsKeyDown(KEY_SPACE)) { state->GameCamera.position.y += moveSpeed * dt; state->GameCamera.invalidate(); }
	if (IsKeyDown(KEY_LEFT_SHIFT)) { state->GameCamera.position.y -= moveSpeed * dt; state->GameCamera.invalidate(); }

	// Rotation
	constexpr float yawSpeed = glm::radians(120.0f);
	auto& yaw = state->GameCamera.rotation.y;
	if (IsKeyDown(KEY_Q)) { yaw += yawSpeed * dt; state->GameCamera.invalidate(); }
	if (IsKeyDown(KEY_E)) { yaw -= yawSpeed * dt; state->GameCamera.invalidate(); }

	//Zoom
	const int32_t wheel = std::exchange(gMouseWheel, 0);
	if (wheel != 0) {
		if (true) {
			// Perspective zoom, usually for a 1st-person game ends up in FOV change
			// decrease FOV to zoom in, increase to zoom out
			constexpr float step = glm::radians(2.0f);   // FOV change per notch
			constexpr float minFov = glm::radians(20.0f);
			constexpr float maxFov = glm::radians(179.99f);
			// Adjust the correct FOV field for your camera (e.g., fovY)
			state->GameCamera.fov = glm::clamp(
				state->GameCamera.fov - step * static_cast<float>(wheel),
				minFov, maxFov
			);
		}
		if (false) {
			// Perspective zoom, usually for a 3rd-person game ends up in camera position change
			// in relation to the player
			constexpr float zoomPerNotch = 2.0f; // units per wheel step
			const float zoomDelta = static_cast<float>(wheel) * zoomPerNotch;
			state->GameCamera.position += forward * zoomDelta;
			state->GameCamera.invalidate();
		}
	}

	// Toggle mouse aim - use IsKeyPressedThisFrame for toggle action
	if (IsKeyPressedThisFrame(KEY_T)) {
		g_MouseAimEnabled = !g_MouseAimEnabled;

		if (g_MouseAimEnabled) {
			// Reset virtual cursor to center when enabling mouse aim
			g_MouseX = state->Settings->windowWidth / 2.0;
			g_MouseY = state->Settings->windowHeight / 2.0;
		}

		SM_TRACE("[GAMEDLL] Mouse Aim %s", g_MouseAimEnabled ? "Enabled" : "Disabled")
	}

	HandleFreeLook(state);
}

void HandleFreeLook(GameState* state) {
	if (!g_MouseAimEnabled) return;

	static double lastMouseX = state->Settings->windowWidth / 2.0;
	static double lastMouseY = state->Settings->windowHeight / 2.0;

	// Compute delta from last frame
	const double dx = g_MouseX - lastMouseX;
	const double dy = g_MouseY - lastMouseY;

	// Update last position
	lastMouseX = g_MouseX;
	lastMouseY = g_MouseY;

	// Mouse sensitivity (adjust to taste)
	constexpr float sensitivity = 0.002f; // radians per pixel

	// Apply deltas directly to yaw/pitch
	state->GameCamera.rotation.y -= static_cast<float>(dx) * sensitivity; // yaw (left/right)
	state->GameCamera.rotation.x -= static_cast<float>(dy) * sensitivity; // pitch (up/down)

	// Clamp pitch to prevent flipping
	constexpr float pitchLimit = glm::radians(89.0f);
	state->GameCamera.rotation.x = glm::clamp(
		state->GameCamera.rotation.x,
		-pitchLimit,
		pitchLimit
	);

	state->GameCamera.rotation.z = 0.0f; // no roll

	state->GameCamera.invalidate();
}

void GameResize(uint32_t width, uint32_t height) {
    SM_TRACE("[GAMEDLL] GameResize: %ux%u", width, height);
}

void GameExit() {
	if (g_PlatformDebugBreak) {
		g_PlatformDebugBreak = nullptr;
	}
    SM_TRACE("[GAMEDLL] GameExit");
}


void DrainInput(SpscRing<InputEvent, ApplicationContext::InputRingSize>* inputRing) {
	static double g_LastMouseX = 0.0, g_LastMouseY = 0.0;
	static bool g_FirstMouse = true;

	InputEvent ev{};
	while (inputRing->Pop(ev))
	{
		if (ev.Type == InputEventType::Key && ev.KeyEvent.Key == KEY_ESCAPE && ev.KeyEvent.Action == RELEASE)
		{
			SM_TRACE("[GAMEDLL] GameUpdate: Shutdown requested via ESC key")
			g_GameState->QuitRequested = true;
			break;
		}

		if (ev.Type == InputEventType::Key) {
			const auto k = static_cast<int>(ev.KeyEvent.Key);
			if (k >= 0 && k <= KEY_LAST) {
				// Track key down state (for continuous checks like movement)
				if (ev.KeyEvent.Action == PRESS || ev.KeyEvent.Action == REPEAT) {
					gKeysDown[k] = true;
				}
				if (ev.KeyEvent.Action == RELEASE) {
					gKeysDown[k] = false;
				}
				
				// Track single-frame press (only on PRESS, not REPEAT)
				if (ev.KeyEvent.Action == PRESS) {
					gKeysPressedThisFrame[k] = true;
				}
			}
		}

		if (ev.Type == InputEventType::MouseMove) {
			if (g_MouseAimEnabled) {
				if (g_FirstMouse) {
					g_LastMouseX = ev.MouseMoveEvent.X;
					g_LastMouseY = ev.MouseMoveEvent.Y;
					g_FirstMouse = false;
				}

				double dx = ev.MouseMoveEvent.X - g_LastMouseX;
				double dy = ev.MouseMoveEvent.Y - g_LastMouseY;

				// Optional: clamp large jumps to prevent teleportation
				constexpr double maxDelta = 100.0;
				dx = glm::clamp(dx, -maxDelta, maxDelta);
				dy = glm::clamp(dy, -maxDelta, maxDelta);

				// Just accumulate for smoothing (not for ray-casting anymore)
				g_MouseX += dx;
				g_MouseY += dy;

				g_LastMouseX = ev.MouseMoveEvent.X;
				g_LastMouseY = ev.MouseMoveEvent.Y;
			} else {
				g_MouseX = ev.MouseMoveEvent.X;
				g_MouseY = ev.MouseMoveEvent.Y;
				g_FirstMouse = true;
			}
		}

		if (ev.Type == InputEventType::MouseWheel) {
			gMouseWheel = static_cast<int32_t>(ev.MouseScrollEvent.OffsetY);
		}
	}

	//static char utf8[5] = {}; // Max 4 bytes + null terminator
	//int len = EncodeUTF8(code, utf8);
	//SM_TRACE("Char: %.*s", len, utf8);
}

inline int EncodeUTF8(unsigned int codepoint, char* out) {
	if (codepoint <= 0x7F) {
		out[0] = static_cast<char>(codepoint);
		return 1;
	} else if (codepoint <= 0x7FF) {
		out[0] = static_cast<char>(0xC0 | (codepoint >> 6));
		out[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
		return 2;
	} else if (codepoint <= 0xFFFF) {
		out[0] = static_cast<char>(0xE0 | (codepoint >> 12));
		out[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
		out[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
		return 3;
	} else if (codepoint <= 0x10FFFF) {
		out[0] = static_cast<char>(0xF0 | (codepoint >> 18));
		out[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
		out[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
		out[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
		return 4;
	}
	return 0; // Invalid codepoint
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