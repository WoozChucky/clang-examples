#include "Game.h"
#include "Systems.h"
#include "PlayerMovement.h"
#include "CameraFollow.h"
#include "MenuHitTest.h" // ToUiSpace + PointInRect
#include "StateScope.h"  // ScopeAllows
#include "Actions.h"     // ActionCategory / Actions::

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
        // Gameplay runs only in-level.
        const auto* gs = ctx.world.GetSingleton<GameStateComponent>();
        if (!gs || gs->Current != GameStateId::InLevel) return;

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

        // Eye distance: the zoom singleton when present, else the static default. The
        // zoom system runs first (registered before this) so the wheel takes effect
        // the same tick. Copy the constant params and override only Distance.
        FollowCameraParams params = kPoE2Follow;
        if (const auto* z = ctx.world.GetSingleton<CameraZoomComponent>())
            params.Distance = z->Distance;

        const CameraView cam = ComputeFollowCamera(target, params, aspect);
        ctx.world.ModifySingleton<WorldCameraComponent>([&](WorldCameraComponent& w) {
            w.View       = cam.View;
            w.Projection = cam.Projection;
            w.Position   = cam.Position;
        });
    }
    const char* Name() const override { return "IsometricFollowCameraSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};

// Mouse-wheel zoom for the isometric follow camera. Adjusts the persistent eye
// distance (CameraZoomComponent) only; the follow system rebuilds the view matrix
// from it, so this never touches the camera transform directly. Runs before the
// follow system so a notch applies the same tick.
class CameraZoomSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        // Gameplay runs only in-level.
        const auto* gs = ctx.world.GetSingleton<GameStateComponent>();
        if (!gs || gs->Current != GameStateId::InLevel) return;

        const auto* in = ctx.world.GetSingleton<InputStateComponent>();
        if (!in || in->Wheel == 0) return; // Wheel is a per-tick delta (reset each tick)

        // Seed the singleton on first use so ModifySingleton has a target.
        if (!ctx.world.GetSingleton<CameraZoomComponent>())
            ctx.world.SetSingleton(CameraZoomComponent{ kPoE2Follow.Distance });

        ctx.world.ModifySingleton<CameraZoomComponent>([&](CameraZoomComponent& z) {
            // Wheel up (+) zooms in (smaller distance). Discrete notch -> no dt scaling.
            z.Distance = glm::clamp(z.Distance - static_cast<float>(in->Wheel) * kZoomStep,
                                    kMinDistance, kMaxDistance);
        });
    }
    const char* Name() const override { return "CameraZoomSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
private:
    static constexpr float kZoomStep    = 2.0f;  // distance units per wheel notch
    static constexpr float kMinDistance = 6.0f;
    static constexpr float kMaxDistance = 40.0f;
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
        // Gameplay runs only in-level.
        const auto* gs = ctx.world.GetSingleton<GameStateComponent>();
        if (!gs || gs->Current != GameStateId::InLevel) return;

        const auto* in = ctx.world.GetSingleton<InputStateComponent>();
        if (!in) return;
        const float dt = static_cast<float>(ctx.dt);
        // EntityId-only callback (no live component refs) so Modify<TransformComponent>
        // below can safely clone/realloc the queried array without invalidating a ref.
        ctx.world.Each<PlayerComponent, TransformComponent>([&](EntityId e) {
            float speed = 5.0f;
            if (const auto* p = ctx.world.GetComponent<PlayerComponent>(e)) speed = p->MoveSpeed;
            // Align movement to the isometric camera yaw so W = "up the screen".
            const glm::vec3 delta = ComputePlanarMove(*in, speed, dt, glm::radians(kPoE2Follow.YawDeg));
            if (delta.x != 0.0f || delta.z != 0.0f)
                ctx.world.Modify<TransformComponent>(e, [&](TransformComponent& t){ t.Position += delta; });
        });
    }
    const char* Name() const override { return "PlayerMovementSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};

// Hit-tests scoped menu buttons against the UI-space mouse, drives their UIRectComponent.Color
// (Normal/Hover/Press), and on a press-inside -> release-inside click pushes an ActionEvent.
// Runs before AppFlowSystem so the action is consumed the same tick. Scope filtering means it's
// inert in states with no scoped buttons (e.g. InLevel).
class MenuInteractionSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const auto* in = ctx.world.GetSingleton<InputStateComponent>();
        if (!in) return;

        GameStateId cur = GameStateId::MainMenu;
        if (const auto* gs = ctx.world.GetSingleton<GameStateComponent>()) cur = gs->Current;

        const auto* vp = ctx.world.GetSingleton<ViewportComponent>();
        const uint32_t ox = vp ? vp->OriginX : 0u;
        const uint32_t oy = vp ? vp->OriginY : 0u;
        const glm::vec2 mouse = ToUiSpace(in->MouseX, in->MouseY, ox, oy);
        const bool down    = in->MouseDown[MOUSE_BUTTON_LEFT];
        const bool pressed = in->MousePressed[MOUSE_BUTTON_LEFT];

        // Ensure the runtime singleton exists, then read the armed button.
        if (!ctx.world.GetSingleton<MenuStateComponent>())
            ctx.world.SetSingleton(MenuStateComponent{});
        EntityId armed = 0;
        if (const auto* ms = ctx.world.GetSingleton<MenuStateComponent>()) armed = ms->ArmedButton;

        auto scopeVisible = [&](EntityId e) {
            const auto* sc = ctx.world.GetComponent<StateScopeComponent>(e);
            return !sc || ScopeAllows(sc->StateMask, cur);
        };
        auto rectOf = [&](EntityId e, glm::vec2& pos, glm::vec2& size) {
            const auto* tr = ctx.world.GetComponent<TransformComponent>(e);
            const auto* rc = ctx.world.GetComponent<UIRectComponent>(e);
            if (!tr || !rc) return false;
            pos = glm::vec2(tr->Position.x, tr->Position.y);
            size = rc->Size;
            return true;
        };

        // 1) Resolve a release of the armed button (armed set on a previous tick, now up).
        uint32_t firedAction = 0; EntityId firedSrc = 0;
        if (armed != 0 && !down) {
            glm::vec2 pos, size;
            if (scopeVisible(armed) && rectOf(armed, pos, size) && PointInRect(mouse, pos, size)) {
                if (const auto* mb = ctx.world.GetComponent<MenuButtonComponent>(armed)) {
                    firedAction = mb->ActionId;
                    firedSrc = armed;
                }
            }
            armed = 0;
        }

        // 2) Hover/press colors + arm-on-press over scoped buttons.
        ctx.world.Each<MenuButtonComponent, UIRectComponent, TransformComponent>([&](EntityId e) {
            if (!scopeVisible(e)) return;
            glm::vec2 pos, size;
            if (!rectOf(e, pos, size)) return;
            const bool inside = PointInRect(mouse, pos, size);
            if (pressed && inside) armed = e;
            const auto* mb = ctx.world.GetComponent<MenuButtonComponent>(e);
            glm::vec4 col = mb->Normal;
            if (inside) col = (down && armed == e) ? mb->Press : mb->Hover;
            ctx.world.Modify<UIRectComponent>(e, [&](UIRectComponent& r){ r.Color = col; });
        });

        // 3) Persist armed state + emit the click action (consumed by AppFlowSystem this tick).
        ctx.world.ModifySingleton<MenuStateComponent>([&](MenuStateComponent& m){ m.ArmedButton = armed; });
        if (firedAction != 0) {
            ctx.world.ModifySingleton<ActionQueueComponent>([&](ActionQueueComponent& q){
                q.Events.push_back(ActionEvent{ firedAction, firedSrc, 0 });
            });
        }
    }
    const char* Name() const override { return "MenuInteractionSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};

// Owns game-state transitions. Drains the per-tick ActionQueue (producers arrive in Phase 4)
// and routes navigation actions. TEMP (Phase 1): toggles MainMenu<->InLevel on TAB so the
// gameplay gating is testable before the menu exists — remove the TAB block in Phase 4.
class AppFlowSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        if (const auto* q = ctx.world.GetSingleton<ActionQueueComponent>()) {
            for (const ActionEvent& e : q->Events) {
                // Phase 4: route by CategoryOf(e.ActionId) and apply Nav transitions here.
                (void)e;
            }
        }

        // TEMP (Phase 1 testability) — remove in Phase 4.
        const auto* in = ctx.world.GetSingleton<InputStateComponent>();
        if (in && in->Pressed[KEY_TAB]) {
            ctx.world.ModifySingleton<GameStateComponent>([](GameStateComponent& s) {
                s.Current = (s.Current == GameStateId::InLevel)
                              ? GameStateId::MainMenu : GameStateId::InLevel;
            });
        }
    }
    const char* Name() const override { return "AppFlowSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};

} // namespace

void GameRegisterSystems(SystemScheduler* s) {
    if (!s) return;
    s->Register(std::make_unique<TextRotationSystem>());
    s->Register(std::make_unique<DayNightSystem>());
    s->Register(std::make_unique<MenuInteractionSystem>());  // emits actions; before AppFlow
    s->Register(std::make_unique<AppFlowSystem>());   // owns transitions; runs before gameplay
    s->Register(std::make_unique<DebugSpawnSystem>());
    s->Register(std::make_unique<PlayerMovementSystem>());
    s->Register(std::make_unique<CameraZoomSystem>());          // before follow: sets distance same tick
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

    // Mirror authoritative ECS state onto the host field (legacy/editor reads). Absent on the
    // very first tick (seeded in the boot block below).
    if (const auto* gs = g_GameState->World.GetSingleton<GameStateComponent>())
        g_GameState->StateId = gs->Current;

    // Clear the per-tick action queue; producers push fresh events this tick.
    if (g_GameState->World.GetSingleton<ActionQueueComponent>())
        g_GameState->World.ModifySingleton<ActionQueueComponent>(
            [](ActionQueueComponent& q){ q.Events.clear(); });

    // Note: input/camera/quit/spawn + day/night + text rotation are now handled by
    // systems (IsometricFollowCameraSystem / QuitRequestSystem / DebugSpawnSystem /
    // DayNightSystem / TextRotationSystem) driven by the SystemScheduler.

    // One-time boot: seed singletons + the starting scene, then enter MainMenu. After this,
    // GameStateComponent.Current is authoritative (AppFlowSystem drives transitions).
    if (g_GameState->StateId == GameStateId::Uninitialized) {
        SM_TRACE("[GAMEDLL] Initializing game...");

        // Game-owned singletons.
        g_GameState->World.SetSingleton(WorldCameraComponent{});
        g_GameState->World.SetSingleton(AppControlComponent{});
        g_GameState->World.SetSingleton(AtmosphereStateComponent{});
        g_GameState->World.SetSingleton(GameStateComponent{ GameStateId::MainMenu });
        g_GameState->World.SetSingleton(ActionQueueComponent{});
        g_GameState->World.SetSingleton(MenuStateComponent{});

        // Persisted scene atmosphere may already be set by a startup world.json load; seed
        // defaults only when absent so loaded values aren't clobbered.
        if (!g_GameState->World.GetSingleton<DayNightConfigComponent>())
            g_GameState->World.SetSingleton(DayNightConfigComponent{});
        if (!g_GameState->World.GetSingleton<FogComponent>())
            g_GameState->World.SetSingleton(FogComponent{});
        if (!g_GameState->World.GetSingleton<SkyComponent>())
            g_GameState->World.SetSingleton(SkyComponent{});

        // World loaded from world.json is authoritative — skip default spawns to avoid
        // duplicates. Defaults are a fallback scene when no world is present.
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

        g_GameState->StateId = GameStateId::MainMenu;
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
