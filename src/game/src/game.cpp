#include "Game.h"
#include "Systems.h"
#include "PlayerMovement.h"
#include "CameraFollow.h"

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

// Fixed-angle isometric camera (PoE2-style) that follows the player. No controls:
// it places WorldCameraComponent at a constant offset from the first PlayerComponent
// entity and looks at it. No-op (camera holds) when no player exists.
class IsometricFollowCameraSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        bool found = false;
        glm::vec3 target(0.0f);
        // EntityId-only callback (no live component refs) so ModifySingleton below is safe.
        ctx.world.Each<PlayerComponent, TransformComponent>([&](EntityId e) {
            if (found) return;
            if (const auto* t = ctx.world.GetComponent<TransformComponent>(e)) {
                target = t->Position;
                found = true;
            }
        });
        if (!found) return; // no player -> leave the camera where it is

        const auto* vp = ctx.world.GetSingleton<ViewportComponent>();
        const float aspect = (vp && vp->Height) ? float(vp->Width) / float(vp->Height) : 16.0f / 9.0f;

        const CameraView cam = ComputeFollowCamera(target, kPoE2Follow, aspect);
        ctx.world.ModifySingleton<WorldCameraComponent>([&](WorldCameraComponent& w) {
            w.View       = cam.View;
            w.Projection = cam.Projection;
            w.Position   = cam.Position;
        });
    }
    const char* Name() const override { return "IsometricFollowCameraSystem"; }
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

// Moves entities tagged with PlayerComponent on the XZ plane from WASD input.
class PlayerMovementSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const auto* in = ctx.world.GetSingleton<InputStateComponent>();
        if (!in) return;
        const float dt = static_cast<float>(ctx.dt);
        // EntityId-only callback (no live component refs) so Modify<TransformComponent>
        // below can safely clone/realloc the queried array without invalidating a ref.
        ctx.world.Each<PlayerComponent, TransformComponent>([&](EntityId e) {
            float speed = 5.0f;
            if (const auto* p = ctx.world.GetComponent<PlayerComponent>(e)) speed = p->MoveSpeed;
            const glm::vec3 delta = ComputePlanarMove(*in, speed, dt);
            if (delta.x != 0.0f || delta.z != 0.0f)
                ctx.world.Modify<TransformComponent>(e, [&](TransformComponent& t){ t.Position += delta; });
        });
    }
    const char* Name() const override { return "PlayerMovementSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};

} // namespace

void GameRegisterSystems(SystemScheduler* s) {
    if (!s) return;
    s->Register(std::make_unique<TextRotationSystem>());
    s->Register(std::make_unique<DayNightSystem>());
    s->Register(std::make_unique<DebugSpawnSystem>());
    s->Register(std::make_unique<PlayerMovementSystem>());
    s->Register(std::make_unique<IsometricFollowCameraSystem>());
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
    // systems (IsometricFollowCameraSystem / QuitRequestSystem / DebugSpawnSystem /
    // DayNightSystem / TextRotationSystem) driven by the SystemScheduler.

    switch (g_GameState->StateId)
    {

	    case GameStateId::Uninitialized: {
	        SM_TRACE("[GAMEDLL] Initializing game...");
            g_GameState->StateId = GameStateId::MainMenu;

	        // Seed game-owned singletons (resolved camera + app control + day/night state).
	        g_GameState->World.SetSingleton(WorldCameraComponent{});
	        g_GameState->World.SetSingleton(AppControlComponent{});
	        g_GameState->World.SetSingleton(AtmosphereStateComponent{});
	        // Persisted scene atmosphere: a startup world.json load (which runs before the
	        // first GameUpdate) may have already set these from the "Environment" block.
	        // Seed defaults only when absent so loaded values are not clobbered.
	        if (!g_GameState->World.GetSingleton<DayNightConfigComponent>())
	            g_GameState->World.SetSingleton(DayNightConfigComponent{});
	        if (!g_GameState->World.GetSingleton<FogComponent>())
	            g_GameState->World.SetSingleton(FogComponent{});
	        if (!g_GameState->World.GetSingleton<SkyComponent>())
	            g_GameState->World.SetSingleton(SkyComponent{});

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
