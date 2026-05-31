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
#include "WireCodec.h"  // protobuf wire-format encode/decode (plumbing demo)
#include "SessionFlow.h"  // pure client session FSM driven by ClientSessionSystem

#include <netlib/netlib.h> // MakeTcpServer/MakeTcpClient factories for the net demo
#include "ServerControl.h" // kDedicatedServerDefaultPort

#include <memory>
#include <tuple>
#include <cstring>

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
// Dual-server connection flow (prototype; loopback, all in one process).
//
// Adapters are marked gameResident=true: on a Game.dll hot-reload these systems are
// destroyed and recreated, so their handles are lost — the teardown
// (GameThread → NetSubsystem::ReleaseGameResidentConnections) closes them before
// the DLL unloads, and the fresh instances re-create them.

namespace {
// Serialize a protobuf message into a pooled send buffer ([u16 opcode][protobuf])
// and send it — zero extra copy (serialize-in-place via wirecodec::EncodeInto).
template <class M>
bool SendMessage(const NetServices* net, NetHandle h, NetConnId conn, const M& msg) {
    const uint32_t total = 2u + static_cast<uint32_t>(msg.ByteSizeLong());
    SendBuffer sb = net->AcquireSend(total);
    if (!sb.data) return false;
    const uint32_t written = wirecodec::EncodeInto(sb.data, sb.cap, msg);
    if (written == 0) { net->AbortSend(std::move(sb)); return false; }   // cap == total, so this shouldn't happen
    return net->Send(h, conn, std::move(sb), written);
}
} // namespace

// ---- Dual-server connection flow (prototype; loopback, all in one process) ----
namespace flow {
    constexpr uint16_t kAuthPort  = 27100;
    constexpr uint16_t kWorldPort = 27101;
    constexpr const char* kHost   = "127.0.0.1";
}

// Dispatch helper: opcode of a received frame (0 if runt).
static inline uint16_t FrameOpcode(const NetEvent& ev) {
    return wirecodec::PeekOpcode(ev.payload, ev.len);
}

// AuthServer: login (accept any) + world list + world select (returns world addr + token).
class AuthServerSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const NetServices* net = ctx.Net;
        if (!net) return;
        if (!m_Started) {
            m_Started = true;
            NetServerConfig sc{};
            sc.bind = netlib::Endpoint{ flow::kHost, flow::kAuthPort };
            sc.gameResident = true;
            m_Server = net->CreateServer(&netlib::MakeTcpServer, sc);
            if (m_Server != NetHandle::Invalid) SM_TRACE("AuthServer: listening on %s:%u", flow::kHost, (unsigned)flow::kAuthPort);
            else                                SM_WARN("AuthServer: failed to bind %s:%u", flow::kHost, (unsigned)flow::kAuthPort);
        }
        NetEvent ev{};
        while (net->PollEvent(&ev)) {
            if (ev.adapter != m_Server) continue;
            if (ev.kind != NetEventKind::Message) continue;
            const uint16_t op = FrameOpcode(ev);
            if (op == (uint16_t)wire::OPCODE_LOGIN_REQ) {
                wire::LoginReq req;
                if (!wirecodec::Decode(ev.payload + 2, ev.len - 2, req)) { SM_WARN("AuthServer: bad LoginReq"); continue; }
                wire::LoginResp resp; resp.set_ok(true);
                SendMessage(net, m_Server, ev.conn, resp);
                SM_TRACE("AuthServer: login '%s' -> ok (conn %llu)", req.username().c_str(), (unsigned long long)ev.conn);
            } else if (op == (uint16_t)wire::OPCODE_WORLD_LIST_REQ) {
                wire::WorldListResp resp;
                auto* w = resp.add_worlds();
                w->set_id(1); w->set_name("Local World"); w->set_host(flow::kHost); w->set_port(flow::kWorldPort);
                SendMessage(net, m_Server, ev.conn, resp);
                SM_TRACE("AuthServer: sent world list (1 world) to conn %llu", (unsigned long long)ev.conn);
            } else if (op == (uint16_t)wire::OPCODE_WORLD_SELECT_REQ) {
                wire::WorldSelectReq req;
                wirecodec::Decode(ev.payload + 2, ev.len - 2, req);
                wire::WorldSelectResp resp;
                resp.set_ok(true);
                resp.set_host(flow::kHost);
                resp.set_port(flow::kWorldPort);
                resp.set_session_token("sess-" + std::to_string(++m_TokenCounter));
                SendMessage(net, m_Server, ev.conn, resp);
                SM_TRACE("AuthServer: world %u selected -> %s:%u token=%s",
                         req.world_id(), flow::kHost, (unsigned)flow::kWorldPort, resp.session_token().c_str());
            }
        }
    }
    const char* Name() const override { return "AuthServerSystem"; }
    SystemPhase Phase() const override { return SystemPhase::PreRender; }
private:
    bool      m_Started = false;
    NetHandle m_Server  = NetHandle::Invalid;
    uint32_t  m_TokenCounter = 0;
};

// WorldServer: session-token handshake (accept any non-empty) + char list + char select + enter-game.
class WorldServerSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const NetServices* net = ctx.Net;
        if (!net) return;
        if (!m_Started) {
            m_Started = true;
            NetServerConfig sc{};
            sc.bind = netlib::Endpoint{ flow::kHost, flow::kWorldPort };
            sc.gameResident = true;
            m_Server = net->CreateServer(&netlib::MakeTcpServer, sc);
            if (m_Server != NetHandle::Invalid) SM_TRACE("WorldServer: listening on %s:%u", flow::kHost, (unsigned)flow::kWorldPort);
            else                                SM_WARN("WorldServer: failed to bind %s:%u", flow::kHost, (unsigned)flow::kWorldPort);
        }
        NetEvent ev{};
        while (net->PollEvent(&ev)) {
            if (ev.adapter != m_Server) continue;
            if (ev.kind != NetEventKind::Message) continue;
            const uint16_t op = FrameOpcode(ev);
            if (op == (uint16_t)wire::OPCODE_SESSION_AUTH_REQ) {
                wire::SessionAuthReq req;
                wirecodec::Decode(ev.payload + 2, ev.len - 2, req);
                wire::SessionAuthResp resp;
                resp.set_ok(!req.session_token().empty());
                SendMessage(net, m_Server, ev.conn, resp);
                SM_TRACE("WorldServer: session token '%s' -> %s (conn %llu)",
                         req.session_token().c_str(), resp.ok() ? "ok" : "reject", (unsigned long long)ev.conn);
            } else if (op == (uint16_t)wire::OPCODE_CHAR_LIST_REQ) {
                wire::CharListResp resp;
                auto* c0 = resp.add_chars(); c0->set_id(1); c0->set_name("Hero");  c0->set_level(10);
                auto* c1 = resp.add_chars(); c1->set_id(2); c1->set_name("Rogue"); c1->set_level(7);
                SendMessage(net, m_Server, ev.conn, resp);
                SM_TRACE("WorldServer: sent char list (2) to conn %llu", (unsigned long long)ev.conn);
            } else if (op == (uint16_t)wire::OPCODE_CHAR_SELECT_REQ) {
                wire::CharSelectReq req;
                wirecodec::Decode(ev.payload + 2, ev.len - 2, req);
                wire::CharSelectResp resp; resp.set_ok(true);
                SendMessage(net, m_Server, ev.conn, resp);
                SM_TRACE("WorldServer: char %u selected (conn %llu)", req.char_id(), (unsigned long long)ev.conn);
            } else if (op == (uint16_t)wire::OPCODE_ENTER_GAME_REQ) {
                wire::EnterGameResp resp; resp.set_ok(true);
                SendMessage(net, m_Server, ev.conn, resp);
                SM_TRACE("WorldServer: enter-game ok (conn %llu)", (unsigned long long)ev.conn);
            }
        }
    }
    const char* Name() const override { return "WorldServerSystem"; }
    SystemPhase Phase() const override { return SystemPhase::PreRender; }
private:
    bool      m_Started = false;
    NetHandle m_Server  = NetHandle::Invalid;
};

// Client: owns BOTH client handles + the session FSM; auto-advances auth -> world -> char -> in-game.
class ClientSessionSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const NetServices* net = ctx.Net;
        if (!net) return;

        // Disconnected: (re)start by connecting the auth client (bounded retry).
        if (m_State == SessionState::Disconnected) {
            m_RetryAccum += ctx.dt;
            const double interval = (m_RetryCount < kMaxFastRetries) ? kFastRetrySec : kSlowRetrySec;
            if (m_RetryAccum < interval) return;
            m_RetryAccum = 0.0;
            CloseClients(net);
            NetClientConfig cc{}; cc.target = netlib::Endpoint{ flow::kHost, flow::kAuthPort }; cc.gameResident = true;
            m_Auth = net->CreateClient(&netlib::MakeTcpClient, cc);
            if (m_RetryCount < kMaxFastRetries) m_RetryCount++;
            if (m_Auth == NetHandle::Invalid) { SM_WARN("ClientSession: auth CreateClient failed"); return; }
            m_State = SessionState::ConnectingAuth;
            SM_TRACE("ClientSession: connecting to auth %s:%u", flow::kHost, (unsigned)flow::kAuthPort);
            return;
        }

        NetEvent ev{};
        while (net->PollEvent(&ev)) {
            const bool isAuth  = (ev.adapter == m_Auth);
            const bool isWorld = (ev.adapter == m_World);
            if (!isAuth && !isWorld) continue;

            if (ev.kind == NetEventKind::Connected) {
                Feed(ctx, net, isAuth ? SessionInput::AuthConnected : SessionInput::WorldConnected);
            } else if (ev.kind == NetEventKind::Disconnected || ev.kind == NetEventKind::Error) {
                Feed(ctx, net, SessionInput::Dropped);
            } else if (ev.kind == NetEventKind::Message) {
                Feed(ctx, net, ClassifyReply(ev));
            }
        }
    }
    const char* Name() const override { return "ClientSessionSystem"; }
    SystemPhase Phase() const override { return SystemPhase::PreRender; }

private:
    SessionInput ClassifyReply(const NetEvent& ev) {
        const uint16_t op = FrameOpcode(ev);
        const uint8_t* body = ev.payload + 2; const uint32_t blen = ev.len - 2;
        switch (op) {
            case (uint16_t)wire::OPCODE_LOGIN_RESP: {
                wire::LoginResp r; return (wirecodec::Decode(body, blen, r) && r.ok()) ? SessionInput::LoginOk : SessionInput::LoginFail;
            }
            case (uint16_t)wire::OPCODE_WORLD_LIST_RESP: {
                wire::WorldListResp r;
                if (wirecodec::Decode(body, blen, r) && r.worlds_size() > 0) { m_PickWorldId = r.worlds(0).id(); return SessionInput::WorldListReceived; }
                return SessionInput::Dropped;
            }
            case (uint16_t)wire::OPCODE_WORLD_SELECT_RESP: {
                wire::WorldSelectResp r;
                if (wirecodec::Decode(body, blen, r) && r.ok()) {
                    m_WorldHost = r.host(); m_WorldPort = (uint16_t)r.port(); m_Token = r.session_token();
                    return SessionInput::WorldSelectOk;
                }
                return SessionInput::WorldSelectFail;
            }
            case (uint16_t)wire::OPCODE_SESSION_AUTH_RESP: {
                wire::SessionAuthResp r; return (wirecodec::Decode(body, blen, r) && r.ok()) ? SessionInput::SessionAuthOk : SessionInput::SessionAuthFail;
            }
            case (uint16_t)wire::OPCODE_CHAR_LIST_RESP: {
                wire::CharListResp r;
                if (wirecodec::Decode(body, blen, r) && r.chars_size() > 0) { m_PickCharId = r.chars(0).id(); return SessionInput::CharListReceived; }
                return SessionInput::Dropped;
            }
            case (uint16_t)wire::OPCODE_CHAR_SELECT_RESP: {
                wire::CharSelectResp r; return (wirecodec::Decode(body, blen, r) && r.ok()) ? SessionInput::CharSelectOk : SessionInput::CharSelectFail;
            }
            case (uint16_t)wire::OPCODE_ENTER_GAME_RESP: {
                wire::EnterGameResp r; return (wirecodec::Decode(body, blen, r) && r.ok()) ? SessionInput::EnterGameOk : SessionInput::EnterGameFail;
            }
            default: return SessionInput::Dropped;
        }
    }

    void Feed(SystemContext& ctx, const NetServices* net, SessionInput in) {
        SessionStep step = AdvanceSession(m_State, in);
        if (step.next != m_State)
            SM_TRACE("ClientSession: state %d -(input %d)-> %d", (int)m_State, (int)in, (int)step.next);
        m_State = step.next;
        DoAction(ctx, net, step.action);
    }

    void DoAction(SystemContext& ctx, const NetServices* net, SessionAction a) {
        switch (a) {
            case SessionAction::SendLogin: {
                wire::LoginReq r; r.set_username("player"); r.set_password("stub");
                SendMessage(net, m_Auth, kNetConnInvalid, r); break;
            }
            case SessionAction::SendWorldListReq: { wire::WorldListReq r; SendMessage(net, m_Auth, kNetConnInvalid, r); break; }
            case SessionAction::SendWorldSelect:  { wire::WorldSelectReq r; r.set_world_id(m_PickWorldId); SendMessage(net, m_Auth, kNetConnInvalid, r); break; }
            case SessionAction::BeginHandoff: {
                if (m_Auth != NetHandle::Invalid) { net->Close(m_Auth); m_Auth = NetHandle::Invalid; }
                NetClientConfig cc{}; cc.target = netlib::Endpoint{ m_WorldHost, m_WorldPort }; cc.gameResident = true;
                m_World = net->CreateClient(&netlib::MakeTcpClient, cc);
                SM_TRACE("ClientSession: handoff -> world %s:%u", m_WorldHost.c_str(), (unsigned)m_WorldPort);
                if (m_World == NetHandle::Invalid) Feed(ctx, net, SessionInput::Dropped);
                break;
            }
            case SessionAction::SendSessionAuth: { wire::SessionAuthReq r; r.set_session_token(m_Token); SendMessage(net, m_World, kNetConnInvalid, r); break; }
            case SessionAction::SendCharListReq: { wire::CharListReq r; SendMessage(net, m_World, kNetConnInvalid, r); break; }
            case SessionAction::SendCharSelect:  { wire::CharSelectReq r; r.set_char_id(m_PickCharId); SendMessage(net, m_World, kNetConnInvalid, r); break; }
            case SessionAction::SendEnterGame:   { wire::EnterGameReq r; SendMessage(net, m_World, kNetConnInvalid, r); break; }
            case SessionAction::EnterGame:
                ctx.world.ModifySingleton<GameStateComponent>([](GameStateComponent& g){ g.Current = GameStateId::InLevel; });
                SM_TRACE("ClientSession: IN GAME - flow complete; GameState -> InLevel");
                break;
            case SessionAction::Reset:
                SM_WARN("ClientSession: flow reset; will retry");
                CloseClients(net);
                m_State = SessionState::Disconnected;
                m_RetryAccum = 0.0;
                break;
            case SessionAction::None: default: break;
        }
    }

    void CloseClients(const NetServices* net) {
        if (m_Auth  != NetHandle::Invalid) { net->Close(m_Auth);  m_Auth  = NetHandle::Invalid; }
        if (m_World != NetHandle::Invalid) { net->Close(m_World); m_World = NetHandle::Invalid; }
    }

    static constexpr int    kMaxFastRetries = 20;
    static constexpr double kFastRetrySec   = 0.5;
    static constexpr double kSlowRetrySec   = 5.0;

    SessionState m_State = SessionState::Disconnected;
    NetHandle    m_Auth  = NetHandle::Invalid;
    NetHandle    m_World = NetHandle::Invalid;
    uint32_t     m_PickWorldId = 0;
    uint32_t     m_PickCharId  = 0;
    std::string  m_WorldHost;
    uint16_t     m_WorldPort   = 0;
    std::string  m_Token;
    int          m_RetryCount  = 0;
    double       m_RetryAccum  = 0.0;
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
    s->Register(std::make_unique<AuthServerSystem>());               // PreRender: dual-server flow — auth (login/world)
    s->Register(std::make_unique<WorldServerSystem>());              // PreRender: dual-server flow — world (session/char/enter)
    s->Register(std::make_unique<ClientSessionSystem>());            // PreRender: dual-server flow — client (drives the FSM)
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

        // One-time protobuf wire-format self-check: exercise EncodeInto (serialize
        // [u16 opcode][protobuf] in place) + PeekOpcode + Decode round-trip. NOT wired
        // into the live net systems here — this is plumbing validation only. Logs
        // (not SM_ASSERT): the game target has no platform_debug_break to link SM_ASSERT.
        {
            wire::Ping ping;
            ping.set_seq(7);
            ping.set_client_time_ms(999);

            uint8_t buf[64];
            const uint32_t n = wirecodec::EncodeInto(buf, sizeof(buf), ping);
            const bool okOp = n >= 2 && wirecodec::PeekOpcode(buf, n) ==
                              static_cast<uint16_t>(wire::OPCODE_PING);
            wire::Ping back;
            const bool okBody = n >= 2 && wirecodec::Decode(buf + 2, n - 2, back);
            if (okOp && okBody && back.seq() == 7u && back.client_time_ms() == 999ull)
                SM_TRACE("[GAMEDLL] protobuf EncodeInto/PeekOpcode/Decode round-trip OK (seq=%u, %u bytes)",
                         back.seq(), n);
            else
                SM_ERROR("[GAMEDLL] protobuf wire round-trip FAILED");

            // New flow messages round-trip (incl. a repeated field).
            wire::WorldListResp wl;
            auto* w = wl.add_worlds();
            w->set_id(1); w->set_name("Local World"); w->set_host("127.0.0.1"); w->set_port(27101);
            uint8_t wbuf[128];
            const uint32_t wn = wirecodec::EncodeInto(wbuf, sizeof(wbuf), wl);
            wire::WorldListResp wlBack;
            const bool wlOk = wn >= 2
                && wirecodec::PeekOpcode(wbuf, wn) == static_cast<uint16_t>(wire::OPCODE_WORLD_LIST_RESP)
                && wirecodec::Decode(wbuf + 2, wn - 2, wlBack)
                && wlBack.worlds_size() == 1 && wlBack.worlds(0).port() == 27101;
            if (wlOk) SM_TRACE("[GAMEDLL] flow-proto round-trip OK (WorldListResp)");
            else      SM_ERROR("[GAMEDLL] flow-proto round-trip FAILED");
        }

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
