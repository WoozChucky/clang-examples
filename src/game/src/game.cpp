#include "Game.h"
#include "Systems.h"

#include <memory>
#include <tuple>

#include <ApplicationContext.h>
#include <Input.h>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace {

// Spins every text entity and cycles its color. (Was GameUpdate/MainMenu.)
class TextRotationSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        ctx.world.Each<TextComponent, TransformComponent>([&](EntityId e) {
            ctx.world.Modify<TransformComponent>(e, [&](auto& transform) {
                constexpr float TWO_PI = 6.28318530718f;
                transform.Rotation.z = fmodf(
                    transform.Rotation.z + glm::radians(180.0f) * static_cast<float>(ctx.dt),
                    TWO_PI);
            });
            ctx.world.Modify<TextComponent>(e, [&](auto& text) {
                const auto time = static_cast<float>(ctx.dt);
                const float red = (sinf(time) + 1.0f) / 2.0f;
                const float green = (cosf(ctx.gameTime) + 1.0f) / 2.0f;
                const float blue = 1.0f - red;
                text.Color = glm::vec4(red, green, blue, 1.0f);
            });
        });
    }
    const char* Name() const override { return "TextRotationSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};

// Drives the SunMarker directional light over a day/night cycle.
// Cycle length is read from the DayNightConfigComponent singleton (default 60.0f).
class DayNightSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        // Tunables (fallback to defaults if the config singleton is missing).
        DayNightConfigComponent cfg{};
        if (const auto* c = ctx.world.GetSingleton<DayNightConfigComponent>()) cfg = *c;

        const float cycle = glm::max(cfg.CycleSeconds, 0.001f);
        const double gameTime = ctx.gameTime;
        const auto phase = static_cast<float>(std::fmod(gameTime, static_cast<double>(cycle)) / static_cast<double>(cycle));
        const float theta = phase * 6.28318530718f;
        const glm::vec3 dir = glm::normalize(glm::vec3(0.0f, -cosf(theta), sinf(theta)));

        const float elevation = glm::clamp(-dir.y, 0.0f, 1.0f); // 1 = noon, 0 = at/below horizon
        const float nightDepth = glm::clamp(dir.y, 0.0f, 1.0f);  // 0 = horizon, 1 = deep midnight
        const float tw = glm::max(cfg.TwilightWidth, 0.001f);

        // Sun: warm at the horizon, white high up; brightness eased + capped, faded to ~0 at night.
        const float dayMix = glm::smoothstep(0.0f, 0.5f, elevation);          // warm -> white
        const glm::vec3 warmColor = glm::vec3(1.00f, 0.68f, 0.35f);
        const glm::vec3 dayColor  = glm::vec3(1.00f, 0.98f, 0.90f);
        const glm::vec3 sunHue    = glm::mix(warmColor, dayColor, dayMix);
        const float sunBright = cfg.DayBrightness * glm::smoothstep(0.0f, tw, elevation); // <= DayBrightness, 0 at night

        ctx.world.Each<SunMarker, LightningComponent>([&](EntityId sun) {
            ctx.world.Modify<LightningComponent>(sun, [&](auto& l) {
                if (l.Type != LightningType::Directional) return;
                l.Direction = glm::vec4(dir, 0.0f);
                l.Color = glm::vec4(sunHue * sunBright, 1.0f);
            });
        });

        // Ambient: small neutral day floor (ramped with the sun) + cool moon fill (ramped with night depth).
        const float dayA   = cfg.DayAmbient * glm::smoothstep(0.0f, tw, elevation);
        const float moonA  = cfg.MoonIntensity * glm::smoothstep(0.0f, tw, nightDepth);
        // Small constant floor so the dusk/dawn terminator never reaches pure black
        // (both the day and moon ramps bottom out at the exact horizon crossing).
        const float kMinAmbient = 0.02f;
        const glm::vec3 ambient = glm::max(glm::vec3(dayA) + cfg.MoonColor * moonA, glm::vec3(kMinAmbient));

        ctx.world.ModifySingleton<AtmosphereStateComponent>([&](AtmosphereStateComponent& a) {
            a.AmbientColor = glm::vec4(ambient, 1.0f);
        });
    }
    const char* Name() const override { return "DayNightSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};

// Free-look fly camera. Reads InputStateComponent + ViewportComponent, advances
// FreeLookControlComponent, and writes the resolved WorldCameraComponent.
// Behavior mirrors the old HandleCameraMovement/HandleFreeLook (now removed).
class FreeLookCameraSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const auto* in = ctx.world.GetSingleton<InputStateComponent>();
        const auto* vp = ctx.world.GetSingleton<ViewportComponent>();
        if (!in || !ctx.world.GetSingleton<FreeLookControlComponent>()) return;
        const float dt = static_cast<float>(ctx.dt);

        ctx.world.ModifySingleton<FreeLookControlComponent>([&](FreeLookControlComponent& c) {
            if (in->Pressed[KEY_T]) c.MouseAimEnabled = !c.MouseAimEnabled;
            if (c.MouseAimEnabled) {
                c.Yaw   -= static_cast<float>(in->MouseDX) * c.Sensitivity;
                c.Pitch -= static_cast<float>(in->MouseDY) * c.Sensitivity;
                const float lim = glm::radians(89.0f);
                c.Pitch = glm::clamp(c.Pitch, -lim, lim);
            }
            const float cp = cosf(c.Pitch), sp = sinf(c.Pitch);
            const float cy = cosf(c.Yaw),   sy = sinf(c.Yaw);
            const glm::vec3 forward = glm::normalize(glm::vec3(-sy, sp*cy, -cp*cy));
            const glm::vec3 right   = glm::normalize(glm::cross(forward, glm::vec3(0,1,0)));
            const float spd = c.MoveSpeed * dt;
            if (in->KeysDown[KEY_W]) c.Position += forward * spd;
            if (in->KeysDown[KEY_S]) c.Position -= forward * spd;
            if (in->KeysDown[KEY_A]) c.Position -= right   * spd;
            if (in->KeysDown[KEY_D]) c.Position += right   * spd;
            if (in->KeysDown[KEY_SPACE])      c.Position.y += spd;
            if (in->KeysDown[KEY_LEFT_SHIFT]) c.Position.y -= spd;
            const float yawSpeed = glm::radians(120.0f) * dt;
            if (in->KeysDown[KEY_Q]) c.Yaw += yawSpeed;
            if (in->KeysDown[KEY_E]) c.Yaw -= yawSpeed;
            if (in->Wheel != 0) {
                const float step = glm::radians(2.0f);
                c.Fov = glm::clamp(c.Fov - step * static_cast<float>(in->Wheel),
                                   glm::radians(20.0f), glm::radians(179.99f));
            }
        });

        const auto* c = ctx.world.GetSingleton<FreeLookControlComponent>();
        const float aspect = (vp && vp->Height) ? float(vp->Width)/float(vp->Height) : 16.0f/9.0f;
        const glm::mat4 T  = glm::translate(glm::mat4(1.0f), -c->Position);
        const glm::mat4 Rx = glm::rotate(glm::mat4(1.0f), -c->Pitch, {1,0,0});
        const glm::mat4 Ry = glm::rotate(glm::mat4(1.0f), -c->Yaw,   {0,1,0});
        ctx.world.ModifySingleton<WorldCameraComponent>([&](WorldCameraComponent& w) {
            w.View       = (Ry * Rx) * T;
            w.Projection = glm::perspectiveRH_ZO(c->Fov, aspect, 0.1f, 1000.0f);
            w.Position   = c->Position;
        });
    }
    const char* Name() const override { return "FreeLookCameraSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};

// Sets AppControlComponent::QuitRequested when the configured quit key is pressed.
class QuitRequestSystem final : public ISystem {
public:
    explicit QuitRequestSystem(int quitKey) : m_QuitKey(quitKey) {}
    void Update(SystemContext& ctx) override {
        const auto* in = ctx.world.GetSingleton<InputStateComponent>();
        if (in && m_QuitKey >= 0 && m_QuitKey <= KEY_LAST && in->Pressed[m_QuitKey])
            ctx.world.ModifySingleton<AppControlComponent>([](AppControlComponent& a){ a.QuitRequested = true; });
    }
    const char* Name() const override { return "QuitRequestSystem"; }
    SystemPhase Phase() const override { return SystemPhase::PostSimulation; }
private:
    int m_QuitKey;
};

// Spawns a bare entity on F12 (debug aid; was the inline F12 block in GameUpdate).
class DebugSpawnSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const auto* in = ctx.world.GetSingleton<InputStateComponent>();
        if (in && in->Pressed[KEY_F12]) {
            const auto id = ctx.world.CreateEntity();
            SM_TRACE("DebugSpawnSystem: spawned entity %llu", (unsigned long long)id);
        }
    }
    const char* Name() const override { return "DebugSpawnSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};

} // namespace

void GameRegisterSystems(SystemScheduler* s) {
    if (!s) return;
    s->Register(std::make_unique<FreeLookCameraSystem>());
    s->Register(std::make_unique<TextRotationSystem>());
    s->Register(std::make_unique<DayNightSystem>());
    s->Register(std::make_unique<DebugSpawnSystem>());
    s->Register(std::make_unique<QuitRequestSystem>(KEY_ESCAPE));
}

static GameState* g_GameState = nullptr;

uint32_t GameGetVersion() {
    return GAME_API_VERSION;
}

void GameUpdate(GameState* state) {
    if (g_GameState != state) {
        g_GameState = state;
		SM_TRACE("[GAMEDLL] State Memory Updated");
	}

    if (!g_GameState) return;

    // Note: input/camera/quit/spawn + day/night + text rotation are now handled by
    // systems (FreeLookCameraSystem / QuitRequestSystem / DebugSpawnSystem /
    // DayNightSystem / TextRotationSystem) driven by the SystemScheduler.

    switch (g_GameState->StateId)
    {

	    case GameStateId::Uninitialized: {
	        SM_TRACE("[GAMEDLL] Initializing game...");
            g_GameState->StateId = GameStateId::MainMenu;

	        // Seed game-owned singletons (camera control + resolved camera + app
	        // control + day/night config). FreeLookControlComponent defaults include
	        // Position {0,5,10} (was g_GameState->GameCamera.position).
	        g_GameState->World.SetSingleton(FreeLookControlComponent{});
	        g_GameState->World.SetSingleton(WorldCameraComponent{});
	        g_GameState->World.SetSingleton(AppControlComponent{});
	        g_GameState->World.SetSingleton(DayNightConfigComponent{});
	        g_GameState->World.SetSingleton(AtmosphereStateComponent{});

	        // World loaded from world.json is authoritative — skip default spawns
	        // to prevent duplicates. Defaults exist only as a Unity-like fallback
	        // scene when no world is present.
	        if (!g_GameState->WorldLoaded) {
	            SM_TRACE("[GAMEDLL] No world loaded — spawning default scene");

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
            break;
        }
	    case GameStateId::InLevel:
		    break;
	    case GameStateId::InEditor:
		    break;
	    case GameStateId::Paused:

		    break;
	    default:
	        SM_ERROR("[GAMEDLL] GameUpdate: Unknown GameStateId %u", static_cast<uint32_t>(g_GameState->StateId));
			break;
    }
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
    SM_TRACE("[GAMEDLL] GameExit");
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
