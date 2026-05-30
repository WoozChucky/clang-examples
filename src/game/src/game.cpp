#include "Game.h"
#include "Systems.h"
#include "PlayerMovement.h"
#include "CameraFollow.h"
#include "Collision.h"
#include "NavObstacleSync.h"
#include "NavAgentSystem.h"
#include "NavClass.h"
#include "MenuHitTest.h" // ToUiSpace + PointInRect
#include "StateScope.h"  // ScopeAllows
#include "Actions.h"     // ActionCategory / Actions::
#include "Atmosphere.h" // SunDirectionFromAngles (static-mode sun direction)

#include <netlib/netlib.h> // MakeTcpServer/MakeTcpClient factories for the net demo
#include "ServerControl.h" // kDedicatedServerDefaultPort

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
            // Leave authored UI/menu text static (don't spin labels, titles, buttons).
            if (ctx.world.HasComponent<StateScopeComponent>(e) ||
                ctx.world.HasComponent<UIRectComponent>(e) ||
                ctx.world.HasComponent<MenuButtonComponent>(e)) return;
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

        // Sun direction: animated by the cycle (DynamicCycle) or pinned at a fixed angle (Static).
        glm::vec3 dir;
        if (cfg.Mode == SkyMode::Static) {
            dir = SunDirectionFromAngles(cfg.StaticSunElevDeg, cfg.StaticSunAzimuthDeg);
        } else {
            const float cycle = glm::max(cfg.CycleSeconds, 0.001f);
            const double gameTime = ctx.gameTime;
            const auto phase = static_cast<float>(std::fmod(gameTime, static_cast<double>(cycle)) / static_cast<double>(cycle));
            const float theta = phase * 6.28318530718f;
            dir = glm::normalize(glm::vec3(0.0f, -cosf(theta), sinf(theta)));
        }

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

        // Eye distance: the zoom singleton when present, else the static default. Both
        // CameraZoom and IsometricFollow live in the PostSimulation phase; within that
        // phase, CameraZoom is registered before this system, so the wheel takes effect
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
    SystemPhase Phase() const override { return SystemPhase::PostSimulation; }
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
    SystemPhase Phase() const override { return SystemPhase::PostSimulation; }
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

// Writes a per-tick MoveIntentComponent for PlayerComponent entities from WASD input.
class PlayerMovementSystem final : public ISystem {
public:
    // Moves entities tagged with PlayerComponent on the XZ plane from WASD input by writing
    // a per-tick MoveIntentComponent. KinematicMovementSystem (Physics phase) resolves the
    // intent against colliders and applies it — this system no longer touches Transform.
    void Update(SystemContext& ctx) override {
        // Gameplay runs only in-level.
        const auto* gs = ctx.world.GetSingleton<GameStateComponent>();
        if (!gs || gs->Current != GameStateId::InLevel) return;

        const auto* in = ctx.world.GetSingleton<InputStateComponent>();
        if (!in) return;
        const float dt = static_cast<float>(ctx.dt);

        ctx.world.Each<PlayerComponent, TransformComponent>([&](EntityId e) {
            float speed = 5.0f;
            if (const auto* p = ctx.world.GetComponent<PlayerComponent>(e)) speed = p->MoveSpeed;
            // Align movement to the isometric camera yaw so W = "up the screen".
            const glm::vec3 desiredDelta = ComputePlanarMove(*in, speed, dt, glm::radians(kPoE2Follow.YawDeg));
            if (desiredDelta.x == 0.0f && desiredDelta.y == 0.0f && desiredDelta.z == 0.0f)
                return;

            // Lazy-seed MoveIntentComponent on first move; subsequent ticks Modify in place.
            // Self-contained: any new mover (AI, projectile) uses the same pattern, no boot
            // coupling required.
            if (!ctx.world.HasComponent<MoveIntentComponent>(e)) {
                ctx.world.AddComponent(e, MoveIntentComponent{});
            }
            ctx.world.Modify<MoveIntentComponent>(e, [&](MoveIntentComponent& m){
                m.DesiredDelta = desiredDelta;
            });
        });
    }
    const char* Name() const override { return "PlayerMovementSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};

// Resolves MoveIntentComponent against ColliderComponent (when present) and applies the
// result to TransformComponent. Single owner of Collision.h calls from movement; producers
// (PlayerMovement today, AI/projectiles later) only write the intent. Clears DesiredDelta
// after consume so a stale value can't re-fire next tick if the producer stops running.
// Physics phase: runs after Simulation (producers) and before PostSimulation (cameras).
// Ungated by GameStateId on purpose: gating belongs in the PRODUCERS (they own when to
// emit intent); the resolver runs every tick and no-ops when no entity has MoveIntent.
// Do NOT add a Current-state gate here, or paused/menu states will leak stale intent.
class KinematicMovementSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        uint8_t classCount = 1;
        float   groundBias = 0.0f;
        if (const auto* navCfg = ctx.world.GetSingleton<NavMeshConfigComponent>()) {
            classCount = NavLiveClassCount(*navCfg);
            // Recast rasterizes the walkable surface UP to ~CellHeight above the real
            // mesh (no detail mesh in the tilecache path). Bias ground-snap down by half
            // a cell so the collider base lands on the true surface in expectation,
            // instead of floating up to a full cell above it.
            groundBias = navCfg->CellHeight * 0.5f;
        }

        ctx.world.Each<TransformComponent, MoveIntentComponent>([&](EntityId e) {
            const auto* intent = ctx.world.GetComponent<MoveIntentComponent>(e);
            if (!intent) return;
            const glm::vec3 desired = intent->DesiredDelta;
            if (desired.x == 0.0f && desired.y == 0.0f && desired.z == 0.0f) return;

            const auto* transform = ctx.world.GetComponent<TransformComponent>(e);
            if (!transform) return;

            const auto* collider = ctx.world.GetComponent<ColliderComponent>(e);

            // Navmesh constraint (opt-in via NavConstrainedComponent): wall-slide
            // the X/Z move along the walkable surface, and ground-snap Y to the
            // surface height (follows ramps/stairs, rests on the floor). Skipped
            // when no marker / no nav table / no mesh built.
            glm::vec3 navDesired = desired;
            bool  groundSnap = false;
            float groundY    = 0.0f;
            if (ctx.world.HasComponent<NavConstrainedComponent>(e)
                && ctx.Nav && ctx.Nav->HasMesh()) {
                const float off = collider ? GroundOffset(*transform, *collider) : 0.0f;
                // Query the navmesh at FOOT level (origin minus the ground offset, i.e.
                // ~the standing surface), NOT the offset center — otherwise the snapped
                // center Y feeds back into the next query and findNearestPoly can flip
                // between stacked walkable surfaces tick-to-tick (vertical jitter).
                const glm::vec3 foot    = transform->Position - glm::vec3(0.0f, off, 0.0f);
                const glm::vec3 footEnd  = foot + glm::vec3(desired.x, 0.0f, desired.z);
                const uint8_t navClass  = ResolveNavClass(ctx.world, e, classCount);
                const glm::vec3 clamped  = ctx.Nav->MoveAlongSurfaceForClass(navClass, foot, footEnd);
                // X/Z from the navmesh wall-slide; Y set directly below (ground-snap),
                // NOT through the AABB resolver — pure floor placement, no vertical physics.
                navDesired = glm::vec3(clamped.x - transform->Position.x,
                                       0.0f,
                                       clamped.z - transform->Position.z);
                groundSnap = true;
                groundY    = clamped.y + off - groundBias;   // counter Recast's half-cell surface lift
            }

            glm::vec3 applied = navDesired;
            if (collider) {
                applied = ResolveKinematicMove(ctx.world, e, *transform, *collider, navDesired).AppliedDelta;
            }

            if (applied.x != 0.0f || applied.y != 0.0f || applied.z != 0.0f || groundSnap) {
                ctx.world.Modify<TransformComponent>(e, [&](TransformComponent& t){
                    t.Position += applied;                 // applied.y is 0 on the nav path; full delta for non-nav movers
                    if (groundSnap) t.Position.y = groundY; // direct ground placement (overrides nav path's 0-Y)
                });
            }

            // Clear-after-consume: zero the intent so a stale value doesn't re-fire next tick
            // if the producer didn't run (e.g. gating turned a movement system off).
            ctx.world.Modify<MoveIntentComponent>(e, [](MoveIntentComponent& m){
                m.DesiredDelta = glm::vec3(0.0f);
            });
        });
    }
    const char* Name() const override { return "KinematicMovementSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Physics; }
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

// Owns game-state transitions. Consumes Nav-category actions from the ActionQueue (emitted by
// MenuInteractionSystem) and applies them; also returns to the menu on ESC while in-level.
class AppFlowSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        if (const auto* q = ctx.world.GetSingleton<ActionQueueComponent>()) {
            for (const ActionEvent& e : q->Events) {
                if (CategoryOf(e.ActionId) != ActionCategory::Nav) continue; // owned elsewhere
                if (e.ActionId == Actions::Play)      SetState(ctx, GameStateId::InLevel);
                else if (e.ActionId == Actions::Back) SetState(ctx, GameStateId::MainMenu);
                else if (e.ActionId == Actions::Quit)
                    ctx.world.ModifySingleton<AppControlComponent>([](AppControlComponent& a){ a.QuitRequested = true; });
            }
        }

        // ESC returns to the menu while in-level (replaces the old always-quit ESC binding).
        const auto* in = ctx.world.GetSingleton<InputStateComponent>();
        const auto* gs = ctx.world.GetSingleton<GameStateComponent>();
        if (in && /*gs && gs->Current == GameStateId::InLevel &&*/ in->Pressed[KEY_ESCAPE])
            ctx.world.ModifySingleton<AppControlComponent>([](AppControlComponent& a){ a.QuitRequested = true; });
            // SetState(ctx, GameStateId::MainMenu);
    }
    const char* Name() const override { return "AppFlowSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
private:
    static void SetState(SystemContext& ctx, GameStateId s) {
        ctx.world.ModifySingleton<GameStateComponent>([&](GameStateComponent& g){ g.Current = s; });
    }
};

} // namespace

// ---------------------------------------------------------------------------
// NetServerSystem / NetClientSystem — minimal loopback networking demo to build on.
//
// Split from the original combined NetDemoSystem along the AppRole boundary: the
// server stands up a local TCP listener (ctx.serverPort) and echoes pings; the
// client connects to it and pings every 2s. Both go through the engine NetServices
// bridge (ctx.Net). The client pings (opcode 1); the server echoes (opcode 2);
// both log what they receive.
//
// Both systems are registered unconditionally; each no-ops for the wrong role
// (server-only on AppRole::Server, client otherwise) — GameRegisterSystems stays
// role-agnostic, role is read per-tick from ctx.role.
//
// Adapters are marked gameResident=true: on a Game.dll hot-reload these systems are
// destroyed and recreated, so their handles are lost — the teardown
// (GameThread → NetSubsystem::ReleaseGameResidentConnections) closes them before
// the DLL unloads, and the fresh instances re-create them. Edit/extend freely:
// swap the loopback target for a real server address, add your own opcodes, etc.

// ---- Server: bind once (to ctx.serverPort), echo pings (opcode 1 -> opcode 2) ----
class NetServerSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const NetServices* net = ctx.Net;
        if (!net) return;                       // nullptr in tests / before engine init
        if (ctx.role != AppRole::Server) return; // client builds skip the listener

        if (!m_ServerStarted) {
            m_ServerStarted = true;
            NetServerConfig sc{};
            sc.bind = netlib::Endpoint{ "127.0.0.1", ctx.serverPort };   // port from the bootstrap (server.exe --port)
            sc.gameResident = true;
            m_Server = net->CreateServer(&netlib::MakeTcpServer, sc);
            if (m_Server != NetHandle::Invalid)
                SM_TRACE("NetDemo[server]: listening on 127.0.0.1:%u", (unsigned)ctx.serverPort);
            else
                SM_WARN("NetDemo[server]: failed to bind 127.0.0.1:%u", (unsigned)ctx.serverPort);
        }
        NetEvent ev{};
        while (net->PollEvent(&ev)) {
            if (ev.adapter != m_Server) continue;
            if (ev.kind == NetEventKind::Message && ev.opcode == 1) {
                net->Send(m_Server, ev.conn, /*opcode*/ 2, ev.payload, ev.len);   // echo
                SM_TRACE("NetDemo[server]: echoed ping from conn %llu", (unsigned long long)ev.conn);
            } else if (ev.kind == NetEventKind::Connected) {
                SM_TRACE("NetDemo[server]: client connected (conn %llu)", (unsigned long long)ev.conn);
            }
        }
    }
    const char* Name() const override { return "NetServerSystem"; }
    SystemPhase Phase() const override { return SystemPhase::PreRender; }

private:
    bool      m_ServerStarted = false;
    NetHandle m_Server        = NetHandle::Invalid;
};

// ---- Client: connect to ctx.serverPort, ping every 2s ----
// Retry is fast for the first kMaxFastRetries, then slows (never permanently gives up) so the
// editor client reconnects whenever a server appears — no editor restart needed. A change to
// ctx.serverPort (the panel's Start writes ApplicationContext::NetServerPort) drops the current
// handle and reconnects fresh to the new port.
class NetClientSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const NetServices* net = ctx.Net;
        if (!net) return;                       // nullptr in tests / before engine init
        if (ctx.role == AppRole::Server) return; // dedicated server runs no client

        const uint16_t port = ctx.serverPort;
        if (port != m_LastPort) {
            if (m_Client != NetHandle::Invalid) net->Close(m_Client);
            m_Client       = NetHandle::Invalid;
            m_Connected    = false;
            m_RetryCount   = 0;
            m_RetryAccum   = 0.0;
            m_GaveUpLogged = false;
            m_LastPort     = port;
        }

        if (m_Client == NetHandle::Invalid) {
            m_RetryAccum += ctx.dt;
            const double interval = (m_RetryCount < kMaxFastRetries) ? kFastRetrySec : kSlowRetrySec;
            if (m_RetryAccum < interval) return;
            m_RetryAccum = 0.0;
            if (m_RetryCount == kMaxFastRetries && !m_GaveUpLogged) {
                SM_WARN("NetDemo[client]: no server on %u after %d tries; slow-retrying every %.0fs",
                        (unsigned)port, kMaxFastRetries, kSlowRetrySec);
                m_GaveUpLogged = true;
            }
            NetClientConfig cc{};
            cc.target = netlib::Endpoint{ "127.0.0.1", port };
            cc.gameResident = true;
            m_Client = net->CreateClient(&netlib::MakeTcpClient, cc);
            if (m_RetryCount < kMaxFastRetries) m_RetryCount++;
            if (m_Client == NetHandle::Invalid)
                SM_WARN("NetDemo[client]: CreateClient failed (port %u)", (unsigned)port);
            else
                SM_TRACE("NetDemo[client]: connecting to 127.0.0.1:%u", (unsigned)port);
            return;
        }

        NetEvent ev{};
        while (net->PollEvent(&ev)) {
            if (ev.adapter != m_Client) continue;
            if (ev.kind == NetEventKind::Connected) {
                m_Connected = true; m_GaveUpLogged = false;
                m_RetryCount = 0;   // replenish budget: bound CONSECUTIVE failures, not lifetime connects
                SM_TRACE("NetDemo[client]: connected");
            } else if (ev.kind == NetEventKind::Message && ev.opcode == 2) {
                SM_TRACE("NetDemo[client]: got echo (%u bytes)", ev.len);
            } else if (ev.kind == NetEventKind::Disconnected || ev.kind == NetEventKind::Error) {
                SM_WARN("NetDemo[client]: connection lost/failed; will retry");
                net->Close(m_Client);
                m_Client = NetHandle::Invalid;
                m_Connected = false;
                m_RetryAccum = 0.0;
                return;
            }
        }

        if (m_Connected) {
            m_PingAccum += ctx.dt;
            if (m_PingAccum >= kPingIntervalSec) {
                m_PingAccum = 0.0;
                const uint8_t payload[4] = { 'p','i','n','g' };
                net->Send(m_Client, kNetConnInvalid, /*opcode*/ 1, payload, sizeof(payload));
                SM_TRACE("NetDemo[client]: sent ping (opcode 1)");
            }
        }
    }
    const char* Name() const override { return "NetClientSystem"; }
    SystemPhase Phase() const override { return SystemPhase::PreRender; }

private:
    static constexpr int    kMaxFastRetries  = 20;
    static constexpr double kFastRetrySec    = 0.5;
    static constexpr double kSlowRetrySec    = 5.0;
    static constexpr double kPingIntervalSec = 2.0;

    NetHandle m_Client        = NetHandle::Invalid;
    bool      m_Connected     = false;
    bool      m_GaveUpLogged  = false;
    int       m_RetryCount    = 0;
    double    m_RetryAccum    = 0.0;
    double    m_PingAccum     = 0.0;
    uint16_t  m_LastPort      = kDedicatedServerDefaultPort;   // detect port change → reconnect
};

void GameRegisterSystems(SystemScheduler* s) {
    if (!s) return;
    s->Register(std::make_unique<TextRotationSystem>());
    s->Register(std::make_unique<DayNightSystem>());
    s->Register(std::make_unique<MenuInteractionSystem>());  // emits actions; before AppFlow
    s->Register(std::make_unique<AppFlowSystem>());   // owns transitions; runs before gameplay
    s->Register(std::make_unique<DebugSpawnSystem>());
    s->Register(std::make_unique<PlayerMovementSystem>());            // Simulation: writes MoveIntent from input
    s->Register(std::make_unique<NavAgentSystem>());                  // Simulation: writes MoveIntent from navmesh path
    s->Register(std::make_unique<KinematicMovementSystem>());         // Physics: resolves intent + applies Transform
    s->Register(std::make_unique<NavObstacleSyncSystem>());           // Physics: syncs NavObstacleComponent → dtTileCache
    s->Register(std::make_unique<CameraZoomSystem>());                // PostSimulation: before follow (sets distance)
    s->Register(std::make_unique<IsometricFollowCameraSystem>());     // PostSimulation: reads post-resolution Transform
    s->Register(std::make_unique<NetServerSystem>());                // PreRender: net demo server half (AppRole::Server only)
    s->Register(std::make_unique<NetClientSystem>());                // PreRender: net demo client half (non-server roles)
}

extern "C" EXPORT_FN void GameInstallLogSink(LogSinkFn fn) { sm_set_log_sink(fn); }

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
    // systems (IsometricFollowCameraSystem / AppFlowSystem / DebugSpawnSystem /
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
        if (!g_GameState->World.GetSingleton<NavMeshConfigComponent>())
            g_GameState->World.SetSingleton(NavMeshConfigComponent{});

        // World loaded from world.json is authoritative — skip default spawns to avoid
        // duplicates. Defaults are a fallback scene when no world is present.
        if (!g_GameState->WorldLoaded) {
            SM_TRACE("[GAMEDLL] No world loaded — spawning default scene");

            // Fallback main menu (no world.json): title + Play/Quit buttons, scoped to MainMenu,
            // so the menu is usable without authoring. Mirrors what you'd author in the editor.
            const uint32_t menuScope = 1u << static_cast<uint32_t>(GameStateId::MainMenu);

            const auto title = g_GameState->World.CreateEntity();
            g_GameState->World.AddComponent(title, TransformComponent{.Position = glm::vec3{200.f, 140.f, 0.f}, .Rotation = glm::vec3{0.f}, .Scale = glm::vec3{1.f}});
            g_GameState->World.AddComponent(title, TextComponent{.Text = "My Game", .Color = glm::vec4{1.0f}, .FontSize = 64});
            g_GameState->World.AddComponent(title, StateScopeComponent{ .StateMask = menuScope });

            auto spawnButton = [&](float x, float y, const char* label, uint32_t action) {
                const auto e = g_GameState->World.CreateEntity();
                g_GameState->World.AddComponent(e, TransformComponent{.Position = glm::vec3{x, y, 0.f}, .Rotation = glm::vec3{0.f}, .Scale = glm::vec3{1.f}});
                g_GameState->World.AddComponent(e, UIRectComponent{ .Size = glm::vec2{220.f, 56.f} });
                g_GameState->World.AddComponent(e, TextComponent{.Text = label, .Color = glm::vec4{1.0f}, .FontSize = 28});
                g_GameState->World.AddComponent(e, MenuButtonComponent{ .ActionId = action });
                g_GameState->World.AddComponent(e, StateScopeComponent{ .StateMask = menuScope });
            };
            spawnButton(200.f, 260.f, "Play", Actions::Play);
            spawnButton(200.f, 330.f, "Quit", Actions::Quit);

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
