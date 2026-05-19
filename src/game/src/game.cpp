#include "Game.h"

#include <tuple>

#include <ApplicationContext.h>
#include <Input.h>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>


static GameState* g_GameState = nullptr;
static bool gKeysPressedThisFrame[KEY_LAST + 1] = {}; // NEW: Track single-frame presses
static int32_t gMouseWheel = 0;

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
    return g_GameState->KeysDown[key];
}

uint32_t GameGetVersion() {
    return GAME_API_VERSION;
}

void GameUpdate(GameState* state) {
    if (g_GameState != state) {
        g_GameState = state;
		SM_TRACE("[GAMEDLL] State Memory Updated")
	}

    if (!g_GameState) return;

	// Clear pressed-this-frame flags at start of each update
	memset(gKeysPressedThisFrame, 0, sizeof(gKeysPressedThisFrame));

	DrainInput(g_GameState->PlatformInput);

	HandleCameraMovement(g_GameState);

    // Note: Day/Night cycle is handled only in MainMenu state (see below)

    {
	    // Use IsKeyPressedThisFrame to add only ONE entity per key press (not per frame while held)
		if (IsKeyPressedThisFrame(KEY_F12)) {
			const auto entityId = g_GameState->World.CreateEntity();
			SM_TRACE("Added new Entity (%llu)", entityId)
		}
    }

    switch (g_GameState->StateId)
    {

	    case GameStateId::Uninitialized: {
	        SM_TRACE("[GAMEDLL] Initializing game...")
            g_GameState->StateId = GameStateId::MainMenu;
	        g_GameState->GameCamera.position = glm::vec3(0.0f, 5.0f, 10.0f);

	        // World loaded from world.json is authoritative — skip default spawns
	        // to prevent duplicates. Defaults exist only as a Unity-like fallback
	        // scene when no world is present.
	        if (!g_GameState->WorldLoaded) {
	            SM_TRACE("[GAMEDLL] No world loaded — spawning default scene")

	            const auto textEntity = g_GameState->World.CreateEntity();
	            g_GameState->World.AddComponent(textEntity, TransformComponent{.Position = glm::vec3{740.f, 250.f, 0.f}, .Rotation = glm::vec3{0.f}, .Scale = glm::vec3{1.f}});
	            g_GameState->World.AddComponent(textEntity, TextComponent{.Text = "Hello, Game!", .Color = glm::vec4{1.0f, 1.0f, 1.0f, 1.0f}, .FontSize = 48});

	            const auto sun = g_GameState->World.CreateEntity();
	            g_GameState->World.AddComponent(sun, TransformComponent{.Position = glm::vec3{0.f, 0.f, 0.f}, .Rotation = glm::vec3{0.f}, .Scale = glm::vec3{1.f}});
	            g_GameState->World.AddComponent(sun, LightningComponent{
	                .Type = LightningType::Directional,
	                .Direction = glm::vec4(glm::normalize(glm::vec3(-1.0f, -1.0f, -1.0f)), 0.0f),
	                .Color = glm::vec4(1.0f, 0.95f, 0.9f, 1.0f)
	            });
	            g_GameState->World.AddComponent(sun, SunMarker{});

	            const auto pointLight = g_GameState->World.CreateEntity();
	            g_GameState->World.AddComponent(pointLight, TransformComponent{.Position = glm::vec3{0.f, 4.f, 0.f}, .Rotation = glm::vec3{0.f}, .Scale = glm::vec3{1.f}});
	            g_GameState->World.AddComponent(pointLight, LightningComponent{
	                .Type = LightningType::Point,
	                .Color = glm::vec4(1.0f, 0.8f, 0.6f, 1.0f),
	                .Intensity = 1.0f,
	                .Range = 3.0f
	            });
	        }

	        break;
	    }
        case GameStateId::MainMenu: {
            // Rotate every text entity + cycle its color. Identity is by component, not handle.
            for (EntityId e : g_GameState->World.View<TextComponent, TransformComponent>()) {
                g_GameState->World.Modify<TransformComponent>(e, [&](auto& transform) {
                    constexpr float TWO_PI = 6.28318530718f;
                    transform.Rotation.z = fmodf(
                        transform.Rotation.z + glm::radians(180.0f) * static_cast<float>(g_GameState->DeltaTime),
                        TWO_PI);
                });
                g_GameState->World.Modify<TextComponent>(e, [&](auto& text) {
                    const auto time = static_cast<float>(g_GameState->DeltaTime);
                    const float red = (sinf(time) + 1.0f) / 2.0f;
                    const float green = (cosf(g_GameState->GameTime) + 1.0f) / 2.0f;
                    const float blue = 1.0f - red;
                    text.Color = glm::vec4(red, green, blue, 1.0f);
                });
            }

            // Day/night cycle drives entities tagged with SunMarker (singleton role).
            for (EntityId sun : g_GameState->World.View<SunMarker, LightningComponent>()) {
                g_GameState->World.Modify<LightningComponent>(sun, [&](auto& l) {
                    if (l.Type != LightningType::Directional) return;

                    const float cycle = glm::max(g_GameState->DayNightCycleSeconds, 0.001f);
                    const double gameTime = g_GameState->GameTime; // seconds
                    const auto phase = static_cast<float>(std::fmod(gameTime, static_cast<double>(cycle)) / static_cast<double>(cycle));
                    const float theta = phase * 6.28318530718f; // 2*pi
                    // Rotate in YZ plane: midday -> light points downward (0, -1, 0)
                    const glm::vec3 dir = glm::normalize(glm::vec3(0.0f, -cosf(theta), sinf(theta)));
                    /*
                     * DO NOT REMOVE THIS COMMENTED OUT CODE - kept for reference
                    const float cycle = glm::max(g_DayNightCycleSeconds, 0.001f);
                    const double gameTime = g_GameState->GameTime; // seconds
                    const auto phase = static_cast<float>(std::fmod(gameTime, static_cast<double>(cycle)) / static_cast<double>(cycle));
                    // Move the light direction from (-X, -Z) to (+X, +Z) over the cycle, with a constant downward Y
                    const float xz = -1.0f + 2.0f * phase; // [-1 .. +1]
                    const glm::vec3 dir = glm::normalize(glm::vec3(xz, -1.0f, xz));
                    */

                    l.Direction = glm::vec4(dir, 0.0f);

                    // Compute sun color over the cycle based on elevation and horizon warmness
                    // Elevation factor: 0 (night) .. 1 (midday). Our light points down at midday (dir.y ~ -1)
                    const float elevation = glm::clamp(-dir.y, 0.0f, 1.0f);
                    // Horizon factor peaks near sunrise/sunset when |dir.y| ~ 0
                    const float horizon = 1.0f - glm::abs(dir.y);
                    const float horizonSmooth = glm::smoothstep(0.0f, 0.5f, horizon);

                    const glm::vec3 dayColor  = glm::vec3(1.00f, 0.98f, 0.90f); // slightly warm white
                    const glm::vec3 warmColor = glm::vec3(1.00f, 0.68f, 0.35f); // sunrise/sunset orange
                    const glm::vec3 nightColor= glm::vec3(0.15f, 0.20f, 0.40f); // cool moonlight blue

                    // Blend between day and warm near horizon, then between night and that by elevation
                    const glm::vec3 dayWarm   = glm::mix(dayColor, warmColor, horizonSmooth);
                    const glm::vec3 baseColor = glm::mix(nightColor, dayWarm, elevation);

                    // Optional brightness scaling to dim the sun at night and brighten at day
                    const float brightness = 0.75f + 0.75f * elevation; // [0.25 .. 1.0]
                    const glm::vec3 finalColor = baseColor * brightness;

                    l.Color = glm::vec4(finalColor, 1.0f);
                });
            }

            break;
        }
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
		g_GameState->MouseAimEnabled = !g_GameState->MouseAimEnabled;

		if (g_GameState->MouseAimEnabled) {
			// Reset virtual cursor to center when enabling mouse aim
			g_GameState->MouseX = state->Settings->windowWidth / 2.0;
			g_GameState->MouseY = state->Settings->windowHeight / 2.0;
		}

		SM_TRACE("[GAMEDLL] Mouse Aim %s", g_GameState->MouseAimEnabled ? "Enabled" : "Disabled")
	}

	HandleFreeLook(state);
}

void HandleFreeLook(GameState* state) {
	if (!g_GameState->MouseAimEnabled) return;

	static double lastMouseX = state->Settings->windowWidth / 2.0;
	static double lastMouseY = state->Settings->windowHeight / 2.0;

	// Compute delta from last frame
	const double dx = g_GameState->MouseX - lastMouseX;
	const double dy = g_GameState->MouseY - lastMouseY;

	// Update last position
	lastMouseX = g_GameState->MouseX;
	lastMouseY = g_GameState->MouseY;

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

void GameExit(GameState* state) {
    // Called on every reload AND on shutdown — keep benign.
    // World is editor-owned and must survive reload.
    if (state && g_GameState == state) {
        g_GameState = nullptr;
    }
    SM_TRACE("[GAMEDLL] GameExit")
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
					g_GameState->KeysDown[k] = true;
				}
				if (ev.KeyEvent.Action == RELEASE) {
					g_GameState->KeysDown[k] = false;
				}

				// Track single-frame press (only on PRESS, not REPEAT)
				if (ev.KeyEvent.Action == PRESS) {
					gKeysPressedThisFrame[k] = true;
				}
			}
		}

		if (ev.Type == InputEventType::MouseMove) {
			if (g_GameState->MouseAimEnabled) {
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
				g_GameState->MouseX += dx;
				g_GameState->MouseY += dy;

				g_LastMouseX = ev.MouseMoveEvent.X;
				g_LastMouseY = ev.MouseMoveEvent.Y;
			} else {
				g_GameState->MouseX = ev.MouseMoveEvent.X;
				g_GameState->MouseY = ev.MouseMoveEvent.Y;
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
