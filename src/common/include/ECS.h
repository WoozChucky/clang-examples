#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <array>
#include <vector>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <typeindex>
#include <string>
#include <algorithm>
#include <thread>
#include <type_traits>
#include <utility>
#include <mutex>
#include <atomic>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "input.h"

// Cross-DLL export annotation. Defined as dllexport in ecs.dll's TU,
// dllimport everywhere else. Allow override (defining ECS_API empty
// externally) for static testing scenarios.
#ifndef ECS_API
  #ifdef _WIN32
    #ifdef ECS_EXPORTS
      #define ECS_API __declspec(dllexport)
    #else
      #define ECS_API __declspec(dllimport)
    #endif
  #else
    #define ECS_API
  #endif
#endif

// #############################################################################
//                           Entity & Component IDs
// #############################################################################

using EntityId = uint64_t;
constexpr EntityId INVALID_ENTITY = 0;

// #############################################################################
//                           Component Types (Examples)
// #############################################################################

struct TransformComponent {
    glm::vec3 Position{0.0f, 0.0f, 0.0f};
    glm::vec3 Rotation{0.0f, 0.0f, 0.0f};
    glm::vec3 Scale{1.0f, 1.0f, 1.0f};
};

struct MeshComponent {
    uint32_t MeshId = 0;
    bool Visible = false;
};

struct SubMesh {
    uint32_t IndexStart = 0;
    uint32_t IndexCount = 0;
    uint32_t MaterialIndex = 0;
};

struct MaterialComponent {
    uint32_t MaterialId = 0;
    glm::vec4 BaseColor{1.0f};
    // Bit flags controlling material behavior
    // bit 0 (1): UseTexture — if set, renderer should sample a texture
    // Additional bits reserved for future use
    uint32_t Flags = 0;
};

struct TextComponent {
    std::string Text;
    glm::vec4 Color{1.0f};
    // Reserved for future use
    //std::string Font;
    size_t FontSize = 12;
};

enum class LightningType : uint8_t {
    Directional = 0,
    Point = 1,
    Spot = 2
};

struct LightningComponent {
    LightningType Type = LightningType::Directional;
    glm::vec4 Direction{0.0f, -1.0f, 0.0f, 0.0f}; // for directional lights
    glm::vec4 Color{1.0f};
    float Intensity = 1.0f;
    float Range = 1.0f;
};

struct ParentComponent {
    EntityId Parent = INVALID_ENTITY;
};

struct ChildComponent {
    std::unordered_set<EntityId> Children;
};

// Zero-size marker tagging the singleton directional light driven by the day/night cycle.
struct SunMarker {};

struct InputStateComponent {
    bool    KeysDown[KEY_LAST + 1] = {};
    bool    Pressed[KEY_LAST + 1]  = {};   // pressed this tick (cleared each drain)
    double  MouseX = 0.0, MouseY = 0.0;
    double  MouseDX = 0.0, MouseDY = 0.0;
    int32_t Wheel = 0;
    bool    MouseDown[MOUSE_BUTTON_LAST + 1]    = {};   // held this tick
    bool    MousePressed[MOUSE_BUTTON_LAST + 1] = {};   // pressed this tick (cleared each drain)
};
struct WorldCameraComponent {
    glm::mat4 View{1.0f};
    glm::mat4 Projection{1.0f};
    glm::vec3 Position{0.0f};
};
// Persistent zoom state for the isometric follow camera (singleton). Mutated by the
// zoom system from the mouse wheel; read by the follow system as its eye distance.
// Lives here (not a file-static) so it survives game-DLL hot-reload and ECS Clear().
struct CameraZoomComponent {
    float Distance = 22.0f; // matches kPoE2Follow.Distance default; clamped by the zoom system
};
struct UICameraComponent {
    glm::mat4 View{1.0f};
    glm::mat4 Projection{1.0f};
};
// Selects how the sky/sun behave. DynamicCycle animates the sun over time;
// Static freezes it at a fixed angle (still lit, still casts shadows).
enum class SkyMode : int { DynamicCycle = 0, Static = 1 };

struct DayNightConfigComponent {
    float     CycleSeconds  = 60.0f;                 // full day length (was 10)
    float     DayBrightness = 1.0f;                  // peak sun brightness (cap <= 1.0, no blow-out)
    float     MoonIntensity = 0.15f;                 // night ambient strength
    float     TwilightWidth = 0.25f;                 // smoothstep band for dawn/dusk easing (elevation units)
    float     DayAmbient    = 0.08f;                 // neutral daytime ambient floor
    glm::vec3 MoonColor     = glm::vec3(0.10f, 0.14f, 0.26f); // cool blue night fill

    // --- Sky mode (added 2026-05-28) ---
    SkyMode   Mode                = SkyMode::DynamicCycle; // default preserves the animated cycle
    float     StaticSunElevDeg    = 50.0f;           // 0 = horizon, 90 = overhead (Static only)
    float     StaticSunAzimuthDeg = 30.0f;           // compass angle around +Y (Static only)
    bool      ShowSunDisc         = true;            // draw the sun disc/halo (mainly for Static)
};

// Computed per-tick by DayNightSystem (game), read by the deferred LightingRenderPass.
struct AtmosphereStateComponent {
    glm::vec4 AmbientColor = glm::vec4(0.08f, 0.08f, 0.08f, 1.0f); // rgb = omnidirectional ambient
};

// Scene-authored fog. Singleton component (one per world), serialized in world.json.
// Defaults reproduce the previous FogSettings values exactly.
struct FogComponent {
    bool      Enabled      = true;
    float     DayDensity   = 0.0f;                            // no fog at full day
    float     NightDensity = 0.09f;                           // noticeable at night
    glm::vec3 DayColor     = glm::vec3(0.60f, 0.70f, 0.80f);  // hazy blue-grey
    glm::vec3 NightColor   = glm::vec3(0.03f, 0.04f, 0.08f);  // dark blue
};

// Scene-authored procedural sky. Singleton component, serialized in world.json.
// Defaults reproduce the previous SkySettings values exactly.
struct SkyComponent {
    bool      Enabled       = true;
    glm::vec3 DayZenith     = glm::vec3(0.20f, 0.40f, 0.85f);
    glm::vec3 DayHorizon    = glm::vec3(0.70f, 0.80f, 0.95f);
    glm::vec3 NightZenith   = glm::vec3(0.01f, 0.02f, 0.06f);
    glm::vec3 NightHorizon  = glm::vec3(0.04f, 0.05f, 0.12f);
    glm::vec3 SunColor      = glm::vec3(1.00f, 0.95f, 0.80f);
    float     SunRadiusDeg  = 3.0f;
    float     SunGlow       = 64.0f;   // halo falloff exponent (higher = tighter)
    glm::vec3 MoonColor     = glm::vec3(0.80f, 0.85f, 1.00f);
    float     MoonRadiusDeg = 2.5f;
    float     MoonGlow      = 128.0f;
};
struct AppControlComponent     { bool  QuitRequested = false; };
struct ViewportComponent       { uint32_t Width = 1920; uint32_t Height = 1080; uint32_t OriginX = 0; uint32_t OriginY = 0; };

// Solid screen-space UI quad (authorable). Positioned by TransformComponent (top-left, pixels);
// rendered by UiRenderPass via the UI shader's solid-color path. Size is in pixels.
struct UIRectComponent {
    glm::vec2 Size{160.0f, 48.0f};
    glm::vec4 Color{0.15f, 0.15f, 0.18f, 1.0f};
};

// Scopes an entity to one or more game states (bit i = game state index i; 0 = always-on).
// The UI renderer + menu interaction only act on entities whose scope allows the current state.
struct StateScopeComponent {
    uint32_t StateMask = 0;
};

// Marks a UI-rect entity as a clickable menu button (authored). ActionId is an Actions:: id
// (data binding to behavior; 0 = none). The interaction system drives UIRectComponent.Color
// between Normal/Hover/Press based on the pointer.
struct MenuButtonComponent {
    uint32_t  ActionId = 0;
    glm::vec4 Normal{0.15f, 0.15f, 0.18f, 1.0f};
    glm::vec4 Hover {0.25f, 0.25f, 0.30f, 1.0f};
    glm::vec4 Press {0.35f, 0.35f, 0.42f, 1.0f};
};

// Runtime singleton: the button currently held under a left-press (click latch). 0 = none.
// Not persisted, not authored (seeded in the boot block like ActionQueueComponent).
struct MenuStateComponent {
    EntityId ArmedButton = 0;
};

// Marks the player-controlled entity. Moved by PlayerMovementSystem (game) from input.
struct PlayerComponent {
    float MoveSpeed = 5.0f; // world units / second
};

// Singleton: the current application state as an opaque bit index. The game owns the
// state vocabulary (see the game's GameStates.h); 0 = unset/initial, seeded by the game
// at startup. The engine compares this against StateScopeComponent.StateMask bits.
struct GameStateComponent {
    uint32_t Current = 0;
};

// One queued UI/game action. Producers (menu interaction, Phase 4) push; owning systems
// consume by ActionId. Param is a generic payload; Source is the emitting entity (0 = none).
struct ActionEvent {
    uint32_t ActionId = 0;
    EntityId Source   = 0;
    uint64_t Param    = 0;
};

// Per-tick action queue (singleton). Cleared at the top of each GameUpdate tick and drained
// by consumer systems the same tick. Not persisted.
struct ActionQueueComponent {
    std::vector<ActionEvent> Events;
};

enum class ColliderShape : uint8_t {
    Box,
    Sphere,
    Capsule,
};

struct ColliderComponent {
    ColliderShape Shape = ColliderShape::Box;
    glm::vec3 Size{1.0f};   // Box: half-extents; Sphere: radius in x; Capsule: radius in x + half-height in y.
    glm::vec3 Offset{0.0f}; // Local-space center offset from TransformComponent::Position (rotation ignored by v1 collision).
    bool IsTrigger = false; // Triggers participate in queries/filters but do not block v1 kinematic movement.
    bool IsStatic  = true;  // Static colliders are treated as world blockers by the v1 player collision pass.
    uint32_t Layer = 1u;          // Membership bits for future filtering.
    uint32_t Mask  = 0xffffffffu; // Which layers this collider interacts with.
};

// Per-entity per-tick movement intent: how much the entity WANTS to move this tick.
// Written by movement systems (PlayerMovementSystem, future AI/projectile systems);
// consumed by KinematicMovementSystem which resolves against colliders, applies to
// TransformComponent, and zeroes DesiredDelta (clear-after-consume — no per-tick
// add/remove churn). Runtime-only: not authored, not serialized, not in ECSCommands.
struct MoveIntentComponent {
    glm::vec3 DesiredDelta{0.0f};
};

// Author-driven choice: which geometry source feeds the navmesh voxelizer for this entity.
// Unset is the sentinel — the build pipeline SM_WARNs + skips entities that forgot to pick.
// Forces authoring intent at scene-construction time (no silent fallback).
enum class NavMeshGeometrySource : uint8_t {
    Unset    = 0,  // sentinel: author forgot to pick → SM_WARN + skip at build
    Collider = 1,  // voxelize ColliderComponent triangles
    Mesh     = 2,  // voxelize MeshComponent triangles (CPU-side verts via MeshSystem)
};

// Per-entity opt-in. Tagging an entity = it contributes triangles to the navmesh build.
// AreaId is Recast's per-triangle classification (0-63); default 63 == RC_WALKABLE_AREA.
struct NavMeshSourceComponent {
    uint8_t                AreaId   = 63;
    NavMeshGeometrySource  Geometry = NavMeshGeometrySource::Unset;
};

inline constexpr int kMaxNavClasses = 8;

// One agent-radius class. Fixed-cap + trivially-copyable (no heap) so it rides
// in the ECS snapshot like NavAgentComponent::CachedPath[]. Single source of
// truth for agent footprint — NavMesh::Build reads these directly.
struct NavClassConfig {
    float AgentRadius   = 0.5f;   // agent capsule radius (m) — drives Recast erosion
    float AgentHeight   = 1.8f;   // agent capsule height (m)
    float AgentMaxClimb = 0.4f;   // step-up height (m)
};

// Singleton (one per scene, persisted in world.json Environment block). Recast/Detour
// build knobs — tune per scene. Indoor/outdoor scenes want very different cell sizes.
struct NavMeshConfigComponent {
    float CellSize      = 0.3f;   // voxel XZ size (m)        — shared across classes
    float CellHeight    = 0.2f;   // voxel Y size (m)         — shared
    float AgentMaxSlope = 45.0f;  // max walkable slope (deg) — shared
    float TileSize      = 32.0f;  // tile XZ size (voxels)    — shared
    int   MaxObstacles  = 128;    // dtTileCache pre-alloc    — shared

    // Per-radius classes. Invariant: ClassCount >= 1 (a default config has one
    // class). ClassId on an entity indexes here.
    NavClassConfig Classes[kMaxNavClasses]{};
    uint8_t        ClassCount = 1;
};

// Recast TileCache obstacle shapes. Distinct from ColliderShape because Recast has
// no Sphere obstacle (Cylinder is the radial primitive) and obstacle/collision
// footprints may diverge (e.g., gameplay buffer wider than visual collision).
enum class NavObstacleShape : uint8_t {
    Cylinder = 0,   // dtTileCache::addObstacle(pos, radius, height)
    Box      = 1,   // dtTileCache::addBoxObstacle(bmin, bmax)
};

// Per-entity dynamic nav obstacle. Carves walkable area on the live navmesh via
// dtTileCache. Sync system queues addObstacle when the component appears,
// removeObstacle when it disappears, remove + re-add when Transform.Position
// moves > epsilon. Persisted in world.json so authors can place permanent
// obstacles in the editor.
struct NavObstacleComponent {
    NavObstacleShape Shape = NavObstacleShape::Cylinder;
    // Cylinder: x = radius, y = height (z unused).
    // Box:      half-extents.
    glm::vec3 Size{0.5f, 1.0f, 0.5f};
    // Local-space center offset from Transform.Position (mirrors ColliderComponent).
    glm::vec3 Offset{0.0f};
};

// Per-entity nav agent. Pairs with NavTargetComponent to drive intent-based
// movement: NavAgentSystem queries NavMesh::FindPath each tick, writes
// MoveIntent toward the next path waypoint at MoveSpeed * dt. Pure reader —
// system doesn't mutate this component.
struct NavAgentComponent {
    // ---- v1 user-authored tunables (serialized) ----
    float MoveSpeed      = 3.0f;   // world units / second
    float Radius         = 0.5f;   // agent footprint; matches NavMeshConfigComponent::AgentRadius authoring
    float ReachedEpsilon = 0.10f;  // distance at which target is considered reached; stop emitting MoveIntent

    // ---- v2 cached-path state (runtime, NOT serialized) ----
    static constexpr int kMaxPathPoints = 32;   // path cap; typical Recast string-pull yields 4-16 waypoints
    glm::vec3 CachedPath[kMaxPathPoints]{};
    uint8_t   PathCount       = 0;              // valid waypoints in CachedPath
    uint8_t   PathIndex       = 0;              // next waypoint the agent walks toward
    glm::vec3 LastTarget{0.0f};                 // target-change detection (epsilon compare vs NavTarget.Destination)
    uint32_t  LastNavVersion  = 0;              // navmesh-rebuild invalidation (vs NavServices::NavVersion)
    float     TimeSinceRepath = 0.0f;           // safety-timer accumulator; repath when > kRepathInterval
};

// Per-entity destination. When attached to a NavAgent entity, system pathfinds
// toward Destination and writes MoveIntent. Game code sets/removes this to
// command the agent. Reaching the destination does NOT remove this component —
// gameplay decides (patrol systems keep it, single-move AI removes it).
struct NavTargetComponent {
    glm::vec3 Destination{0.0f};
};

// Opt-in marker: KinematicMovementSystem clamps this entity's per-tick move to
// the walkable navmesh (wall-slide via NavServices::MoveAlongSurface) before the
// AABB collider resolve. No fields. Added to directly-controlled movers (the
// player). Without it, movement is not navmesh-constrained (today's behavior).
struct NavConstrainedComponent {};

// Per-entity nav class selector: which class mesh (index into
// NavMeshConfigComponent::Classes) this entity navigates / is constrained to.
// Carried by NavAgents and by the player (alongside NavConstrainedComponent).
// Absent → class 0. Out-of-range ClassId → class 0.
struct NavClassComponent { uint8_t ClassId = 0; };

// X-macro: single source of truth for the set of component types that get
// explicit template instantiations in ecs.dll. Adding a new component type
// requires (1) declaring the struct above, (2) adding an X(NewType) line here,
// (3) registering in ECSCommandProcessor in ECSCommands.h.
#define ECS_FOR_EACH_REGISTERED_COMPONENT(X) \
    X(TransformComponent) \
    X(MeshComponent) \
    X(MaterialComponent) \
    X(TextComponent) \
    X(LightningComponent) \
    X(ParentComponent) \
    X(ChildComponent) \
    X(SunMarker) \
    X(InputStateComponent) \
    X(WorldCameraComponent) \
    X(UICameraComponent) \
    X(DayNightConfigComponent) \
    X(AtmosphereStateComponent) \
    X(FogComponent) \
    X(SkyComponent) \
    X(AppControlComponent) \
    X(ViewportComponent) \
    X(PlayerComponent) \
    X(CameraZoomComponent) \
    X(GameStateComponent) \
    X(ActionQueueComponent) \
    X(UIRectComponent) \
    X(StateScopeComponent) \
    X(MenuButtonComponent) \
    X(MenuStateComponent) \
    X(ColliderComponent) \
    X(MoveIntentComponent) \
    X(NavMeshSourceComponent) \
    X(NavMeshConfigComponent) \
    X(NavObstacleComponent) \
    X(NavAgentComponent) \
    X(NavTargetComponent) \
    X(NavConstrainedComponent) \
    X(NavClassComponent)

// #############################################################################
//                           Component Storage (Type-erased container)
// #############################################################################

// Byte accounting for a component array (read-only diagnostics).
struct ArrayMemory { size_t Used = 0; size_t Reserved = 0; };

class IComponentArray {
public:
    virtual ~IComponentArray() = default;
    virtual void Remove(EntityId entity) = 0;
    [[nodiscard]] virtual bool Has(EntityId entity) const = 0;
    [[nodiscard]] virtual size_t Size() const = 0;

    /**
     * @brief Deep-copies this array. Used by type-erased paths that cannot
     *        invoke MutateArray<T> (e.g. ComponentStore::RemoveAllComponents).
     * @return shared_ptr owning a fresh copy of the array contents.
     */
    [[nodiscard]] virtual std::shared_ptr<IComponentArray> Clone() const = 0;

    [[nodiscard]] virtual ArrayMemory MemoryBytes() const = 0;
};

template<typename T>
class ComponentArray final : public IComponentArray {
public:
    ComponentArray() = default;
    ~ComponentArray() override = default;
    ComponentArray(ComponentArray&&) noexcept = default;
    ComponentArray& operator=(ComponentArray&&) noexcept = default;

    ComponentArray(const ComponentArray& other)
        : m_Components(other.m_Components)
        , m_IndexToEntity(other.m_IndexToEntity)
    {
        m_SparsePages.reserve(other.m_SparsePages.size());
        for (const auto& page : other.m_SparsePages)
            m_SparsePages.push_back(page ? std::make_unique<SparsePage>(*page) : nullptr);
    }

    ComponentArray& operator=(const ComponentArray& other) {
        if (this != &other) {
            ComponentArray tmp(other);
            m_Components    = std::move(tmp.m_Components);
            m_IndexToEntity = std::move(tmp.m_IndexToEntity);
            m_SparsePages   = std::move(tmp.m_SparsePages);
        }
        return *this;
    }

    // Deep-copies src into *this, reusing existing buffer capacity where possible.
    // Result is structurally identical to a copy-constructed clone of src (same dense
    // contents, same per-page null/non-null layout). Used by the array recycle pool.
    void CopyFrom(const ComponentArray& src) {
        m_Components.assign(src.m_Components.begin(), src.m_Components.end());
        m_IndexToEntity.assign(src.m_IndexToEntity.begin(), src.m_IndexToEntity.end());

        m_SparsePages.resize(src.m_SparsePages.size()); // drop any surplus dest pages
        for (size_t i = 0; i < src.m_SparsePages.size(); ++i) {
            if (!src.m_SparsePages[i]) {
                m_SparsePages[i].reset();                                   // match src null slot
            } else if (!m_SparsePages[i]) {
                m_SparsePages[i] = std::make_unique<SparsePage>(*src.m_SparsePages[i]);
            } else {
                *m_SparsePages[i] = *src.m_SparsePages[i];                  // reuse 4 KB buffer
            }
        }
    }

    void Add(const EntityId entity, T component) {
        const uint32_t existingIndex = SparseGet(entity);
        if (existingIndex != kInvalid) {
            m_Components[existingIndex] = component;
            return;
        }
        const uint32_t newIndex = static_cast<uint32_t>(m_Components.size());
        SparseSet(entity, newIndex);
        m_Components.push_back(component);
        m_IndexToEntity.push_back(entity);
    }

    void Remove(const EntityId entity) override {
        const uint32_t indexOfRemoved = SparseGet(entity);
        if (indexOfRemoved == kInvalid) return;
        const uint32_t indexOfLast = static_cast<uint32_t>(m_Components.size() - 1);
        m_Components[indexOfRemoved] = m_Components[indexOfLast];
        const EntityId entityOfLast = m_IndexToEntity[indexOfLast];
        m_IndexToEntity[indexOfRemoved] = entityOfLast;
        SparseSet(entityOfLast, indexOfRemoved);
        SparseClear(entity);
        m_IndexToEntity.pop_back();
        m_Components.pop_back();
    }

    T* Get(const EntityId entity) {
        const uint32_t index = SparseGet(entity);
        return index == kInvalid ? nullptr : &m_Components[index];
    }

    const T* Get(const EntityId entity) const {
        const uint32_t index = SparseGet(entity);
        return index == kInvalid ? nullptr : &m_Components[index];
    }

    [[nodiscard]] bool Has(const EntityId entity) const override {
        return SparseGet(entity) != kInvalid;
    }

    [[nodiscard]] size_t Size() const override {
        return m_Components.size();
    }

    [[nodiscard]] ArrayMemory MemoryBytes() const override {
        ArrayMemory m{};
        m.Used     = m_Components.size()    * sizeof(T)
                   + m_IndexToEntity.size() * sizeof(EntityId);
        m.Reserved = m_Components.capacity()    * sizeof(T)
                   + m_IndexToEntity.capacity() * sizeof(EntityId)
                   + m_SparsePages.capacity()   * sizeof(std::unique_ptr<SparsePage>);
        for (const auto& page : m_SparsePages)
            if (page) m.Reserved += sizeof(SparsePage);   // 4 KB each
        return m;
    }

    [[nodiscard]] std::shared_ptr<IComponentArray> Clone() const override; // defined below, out-of-class (uses the array pool)

    // Iterator access for systems
    std::vector<T>& GetComponents() { return m_Components; }
    const std::vector<T>& GetComponents() const { return m_Components; }

    // Get entity for a component index (useful for systems iterating components)
    [[nodiscard]] EntityId GetEntity(const size_t index) const {
        return index < m_IndexToEntity.size() ? m_IndexToEntity[index] : INVALID_ENTITY;
    }

private:
    // Paged sparse set: EntityId -> dense index. A touched page is a fixed 4 KB
    // (1024 * uint32_t); absent pages stay nullptr. Entity ids are dense + recycled
    // (EntityStore), so page count stays low (~maxLiveId/kPageSize). Clone copies
    // only the non-null pages (memcpy each) — the cheap-COW win over the old hashmap.
    static constexpr uint32_t kInvalid  = UINT32_MAX;
    static constexpr uint32_t kPageSize = 1024;
    using SparsePage = std::array<uint32_t, kPageSize>;

    [[nodiscard]] uint32_t SparseGet(EntityId entity) const {
        const size_t page = static_cast<size_t>(entity / kPageSize);
        if (page >= m_SparsePages.size() || !m_SparsePages[page]) return kInvalid;
        return (*m_SparsePages[page])[entity % kPageSize];
    }
    void SparseSet(EntityId entity, uint32_t denseIndex) {
        const size_t page = static_cast<size_t>(entity / kPageSize);
        if (page >= m_SparsePages.size()) m_SparsePages.resize(page + 1);
        if (!m_SparsePages[page]) {
            m_SparsePages[page] = std::make_unique<SparsePage>();
            m_SparsePages[page]->fill(kInvalid);
        }
        (*m_SparsePages[page])[entity % kPageSize] = denseIndex;
    }
    void SparseClear(EntityId entity) {
        const size_t page = static_cast<size_t>(entity / kPageSize);
        if (page < m_SparsePages.size() && m_SparsePages[page])
            (*m_SparsePages[page])[entity % kPageSize] = kInvalid;
    }

    std::vector<T> m_Components;
    std::vector<EntityId> m_IndexToEntity;
    std::vector<std::unique_ptr<SparsePage>> m_SparsePages;
};

// Declare that ComponentArray<T> is instantiated elsewhere (in ecs.dll's TU).
// Prevents per-TU local instantiation in editor.exe, game.dll, test_ecs.exe;
// they link against ecs.dll's exported copy.
// Suppressed in ecs.dll's own TU (ECS_EXPORTS defined) because the explicit
// instantiation definitions below take precedence and MSVC warns on the
// combination of dllexport + extern on an instantiation (C4910).
#ifndef ECS_EXPORTS
#define ECS_EXTERN_TEMPLATE_DECL(T) extern template class ECS_API ComponentArray<T>;
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_EXTERN_TEMPLATE_DECL)
#undef ECS_EXTERN_TEMPLATE_DECL
#endif

// ---- COW array recycle pool (header so consumer-defined component types
// instantiate their own pool locally; built-in T's pools live in ecs.dll via
// the explicit instantiations). Counters are a single exported instance so the
// editor Memory panel aggregates every module's pools. ----
namespace ecs::detail {

struct ArrayPoolCountersT {
    std::atomic<size_t>   Free{0};
    std::atomic<size_t>   InUse{0};
    std::atomic<size_t>   Created{0};
    std::atomic<uint64_t> Reuses{0};
    void OnCreate()  noexcept { ++InUse; ++Created; }
    void OnReuse()   noexcept { --Free;  ++InUse; ++Reuses; }
    void OnRecycle() noexcept { ++Free;  --InUse; }
};

// Single instance, defined + exported from ecs.dll so all modules share counts.
ECS_API ArrayPoolCountersT& ArrayPoolCounters();

// Per-type free-list of recycled ComponentArray<T>. Mutex-guarded: Recycle fires from a
// shared_ptr deleter on whatever thread drops the last ref (RenderThread when a snapshot
// recycles, GameThread otherwise). Non-leaked Meyers singleton per T; safe because the
// app joins both threads + resets LatestWorldSnapshot before static destruction, so no
// Recycle races the dtor.
template<typename T>
class ComponentArrayPool {
public:
    ComponentArray<T>* Acquire() {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Free.empty()) {
            ComponentArray<T>* a = m_Free.back();
            m_Free.pop_back();
            ArrayPoolCounters().OnReuse();
            return a;
        }
        ArrayPoolCounters().OnCreate();
        return new ComponentArray<T>();
    }
    void Recycle(ComponentArray<T>* a) noexcept {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Free.push_back(a);
        ArrayPoolCounters().OnRecycle();
    }
    ~ComponentArrayPool() { for (ComponentArray<T>* a : m_Free) delete a; }
private:
    std::mutex m_Mutex;
    std::vector<ComponentArray<T>*> m_Free;
};

template<typename T>
ComponentArrayPool<T>& GetArrayPool() { static ComponentArrayPool<T> pool; return pool; }

template<typename T>
std::shared_ptr<ComponentArray<T>> MakePooledClone(const ComponentArray<T>& src) {
    // Wrap BEFORE copying: if CopyFrom throws (e.g. bad_alloc on a grow), the deleter
    // returns the array to the pool, keeping InUse/Free balanced (no leak).
    std::shared_ptr<ComponentArray<T>> arr(
        GetArrayPool<T>().Acquire(),
        [](ComponentArray<T>* p) noexcept { GetArrayPool<T>().Recycle(p); });
    arr->CopyFrom(src);
    return arr;
}

} // namespace ecs::detail

template<typename T>
inline std::shared_ptr<IComponentArray> ComponentArray<T>::Clone() const {
    return ecs::detail::MakePooledClone(*this);
}

// #############################################################################
//                           Component Store (Type registry)
// #############################################################################

class ECS_API ComponentStore {
public:
    template<typename T>
    void RegisterComponent() {
        const auto typeIndex = std::type_index(typeid(T));

        if (m_ComponentArrays.contains(typeIndex)) {
            return; // Already registered
        }

        m_ComponentArrays[typeIndex] = std::make_shared<ComponentArray<T>>();
    }

    template<typename T>
    void AddComponent(EntityId entity, T component) { MutateArray<T>().Add(entity, component); }

    template<typename T>
    void RemoveComponent(EntityId entity) { MutateArray<T>().Remove(entity); }

    template<typename T>
    const T* GetComponent(EntityId entity) const {
        const auto componentArray = GetComponentArray<T>();
        return componentArray ? componentArray->Get(entity) : nullptr;
    }

    template<typename T>
    [[nodiscard]] bool HasComponent(EntityId entity) const {
        auto array = GetComponentArray<T>();
        return array && array->Has(entity);
    }

    template<typename T>
    const ComponentArray<T>* GetComponentArray() const {
        const auto typeIndex = std::type_index(typeid(T));

        const auto it = m_ComponentArrays.find(typeIndex);
        if (it == m_ComponentArrays.end()) {
            return nullptr;
        }

        return static_cast<const ComponentArray<T>*>(it->second.get());
    }

    void RemoveAllComponents(EntityId entity);

    void Cleanup() {
        m_ComponentArrays.clear();
    }

    /**
     * @brief Returns a mutable reference to the component array for type T,
     *        cloning the array on first write per tick to preserve any in-flight
     *        snapshot.
     * @tparam T Component type. Must be CopyConstructible.
     * @return Reference to the master's array for T. Valid until the next
     *         CreateSnapshot or RemoveAllComponents call.
     * @threading GameThread only.
     * @cow First call per tick clones the array; subsequent calls return the
     *      same (already-cloned) array.
     */
    template<typename T>
    ComponentArray<T>& MutateArray() {
        AssertOwnerThread();
        const auto typeIndex = std::type_index(typeid(T));
        auto& slot = m_ComponentArrays[typeIndex];
        if (!slot) slot = std::make_shared<ComponentArray<T>>();
        if (m_DirtyThisTick.insert(typeIndex).second) {
            slot = ecs::detail::MakePooledClone<T>(static_cast<const ComponentArray<T>&>(*slot));
        }
        return static_cast<ComponentArray<T>&>(*slot);
    }

    /**
     * @brief Returns a const pointer to the array for T, or nullptr if no
     *        component of T has been registered yet.
     * @snapshot Safe to call through a snapshot reference; the returned array
     *           is immutable for the snapshot's lifetime.
     */
    template<typename T>
    const ComponentArray<T>* GetArray() const {
        const auto typeIndex = std::type_index(typeid(T));
        const auto it = m_ComponentArrays.find(typeIndex);
        return it == m_ComponentArrays.end()
            ? nullptr
            : static_cast<const ComponentArray<T>*>(it->second.get());
    }

    /**
     * @brief Copies the array map (shared_ptr refcount bumps) from `other`.
     *        Used by CreateSnapshot. The destination's dirty set is left empty.
     */
    void CopyArraysFrom(const ComponentStore& other);

    /**
     * @brief Resets the master's per-tick dirty set after a snapshot is published.
     */
    void ClearDirty();

    [[nodiscard]] ArrayMemory MemoryBytes(size_t& outArrayCount) const {
        ArrayMemory sum{};
        outArrayCount = m_ComponentArrays.size();
        for (const auto& [type, slot] : m_ComponentArrays) {
            const ArrayMemory m = slot->MemoryBytes();
            sum.Used     += m.Used;
            sum.Reserved += m.Reserved;
        }
        return sum;
    }

private:
#ifndef NDEBUG
    std::thread::id m_OwnerThread = std::this_thread::get_id();
    void AssertOwnerThread() const {
        assert(std::this_thread::get_id() == m_OwnerThread &&
               "ECS mutated from non-owner thread");
    }
#else
    void AssertOwnerThread() const {}
#endif

    std::unordered_map<std::type_index, std::shared_ptr<IComponentArray>> m_ComponentArrays;
    std::unordered_set<std::type_index> m_DirtyThisTick;
};

// Per-T extern template declarations for ComponentStore methods. Guarded
// with ECS_EXPORTS so ecs.dll's own TU (which provides the definitions)
// doesn't see the extern declarations.
#ifndef ECS_EXPORTS
#define ECS_EXTERN_COMPONENT_STORE_METHODS(T) \
    extern template ECS_API ComponentArray<T>& ComponentStore::MutateArray<T>(); \
    extern template ECS_API const ComponentArray<T>* ComponentStore::GetArray<T>() const; \
    extern template ECS_API void ComponentStore::AddComponent<T>(EntityId, T); \
    extern template ECS_API void ComponentStore::RemoveComponent<T>(EntityId); \
    extern template ECS_API bool ComponentStore::HasComponent<T>(EntityId) const; \
    extern template ECS_API const T* ComponentStore::GetComponent<T>(EntityId) const;
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_EXTERN_COMPONENT_STORE_METHODS)
#undef ECS_EXTERN_COMPONENT_STORE_METHODS
#endif

// #############################################################################
//                           Entity Store (Entity lifecycle management)
// #############################################################################

class EntityStore {
public:
    EntityId CreateEntity() {
        const EntityId id = m_FreeEntities.empty() ? m_NextEntityId++ : m_FreeEntities.back();
        if (!m_FreeEntities.empty()) m_FreeEntities.pop_back();

        m_ActiveEntities.push_back(id);
        return id;
    }

    // Allocates an id NOT tracked in m_ActiveEntities (the ECS singleton entity).
    // Invisible to GetActiveEntities()/GetEntityCount(); never recycled.
    EntityId CreateReserved() {
        return m_NextEntityId++;
    }

    // Removes all ACTIVE (gameplay) entities; leaves m_NextEntityId untouched so a
    // previously-reserved id stays valid and is never re-handed-out.
    void ClearActive() {
        m_ActiveEntities.clear();
        m_FreeEntities.clear();
    }

    void DestroyEntity(const EntityId entity) {
        const auto it = std::ranges::find(m_ActiveEntities, entity);
        if (it != m_ActiveEntities.end()) {
            m_ActiveEntities.erase(it);
            m_FreeEntities.push_back(entity); // Recycle ID
        }
    }

    [[nodiscard]] bool IsValid(const EntityId entity) const {
        return std::ranges::find(m_ActiveEntities, entity) != m_ActiveEntities.end();
    }

    [[nodiscard]] size_t GetEntityCount() const {
        return m_ActiveEntities.size();
    }

    [[nodiscard]] ArrayMemory MemoryBytes() const {
        ArrayMemory m{};
        m.Used     = (m_ActiveEntities.size()     + m_FreeEntities.size())     * sizeof(EntityId);
        m.Reserved = (m_ActiveEntities.capacity() + m_FreeEntities.capacity()) * sizeof(EntityId);
        return m;
    }

    [[nodiscard]] const std::vector<EntityId>& GetActiveEntities() const {
        return m_ActiveEntities;
    }

    void Clear() {
        m_ActiveEntities.clear();
        m_FreeEntities.clear();
        m_NextEntityId = 1; // Start from 1 (0 is INVALID_ENTITY)
    }

private:
    EntityId                        m_NextEntityId = 1; // 0 reserved for INVALID_ENTITY
    std::vector<EntityId>           m_ActiveEntities;
    std::vector<EntityId>           m_FreeEntities; // Recycled entity IDs
};

// #############################################################################
//                           ECS World (Main API)
// #############################################################################

// Stats for the snapshot object pool (see ecs.cpp). Rendered by the editor Memory panel.
struct SnapshotPoolStats {
    size_t   Free;     // idle ECS objects in the pool free-list
    size_t   InUse;    // currently handed out (live snapshots)
    size_t   Created;  // total ECS objects ever allocated by the pool
    uint64_t Reuses;   // # of Acquire calls served from the free-list
};
ECS_API SnapshotPoolStats GetSnapshotPoolStats();

// Stats for the per-type ComponentArray recycle pool (COW Part 2), aggregated across
// all component types. Rendered by the editor Memory panel.
struct ComponentArrayPoolStats {
    size_t   Free;     // arrays sitting on the free-lists (summed over types)
    size_t   InUse;    // arrays handed out and not yet recycled
    size_t   Created;  // total arrays ever allocated by the pools
    uint64_t Reuses;   // # of Acquire calls served from a free-list
};
ECS_API ComponentArrayPoolStats GetComponentArrayPoolStats();

// Aggregate ECS storage bytes (read-only diagnostics; excludes map/control-block overhead).
struct EcsMemoryStats {
    size_t ComponentUsed = 0;
    size_t ComponentReserved = 0;
    size_t EntityUsed = 0;
    size_t EntityReserved = 0;
    size_t ArrayCount = 0;
    size_t EntityCount = 0;
};

class ECS_API ECS {
public:
    ECS() { m_SingletonEntity = m_EntityStore.CreateReserved(); }

    // Entity management
    EntityId CreateEntity() {
        return m_EntityStore.CreateEntity();
    }

    void DestroyEntity(EntityId entity);

    [[nodiscard]] bool IsValidEntity(const EntityId entity) const {
        return m_EntityStore.IsValid(entity);
    }

    [[nodiscard]] size_t GetEntityCount() const {
        return m_EntityStore.GetEntityCount();
    }

    [[nodiscard]] const std::vector<EntityId>& GetActiveEntities() const {
        return m_EntityStore.GetActiveEntities();
    }

    // Component management
    template<typename T>
    void AddComponent(EntityId entity, T component) { m_ComponentStore.AddComponent<T>(entity, std::move(component)); }

    template<typename T>
    void RemoveComponent(EntityId entity) { m_ComponentStore.RemoveComponent<T>(entity); }

    template<typename T>
    const T* GetComponent(EntityId entity) const { return m_ComponentStore.GetComponent<T>(entity); }

    template<typename T>
    [[nodiscard]] bool HasComponent(EntityId entity) const { return m_ComponentStore.HasComponent<T>(entity); }

    // Multi-component queries
    template<typename... Components>
    [[nodiscard]] bool HasComponents(const EntityId entity) const {
        return (HasComponent<Components>(entity) && ...);
    }

    // Get multiple components at once (returns tuple of const pointers)
    // Usage: if (auto [transform, mesh] = world.GetComponents<TransformComponent, MeshComponent>(entity); transform && mesh) { ... }
    template<typename... Components>
    std::tuple<const Components*...> GetComponents(EntityId entity) const {
        return std::make_tuple(GetComponent<Components>(entity)...);
    }

    // Get component array for system iteration (read-only)
    template<typename T>
    const ComponentArray<T>* GetComponentArray() const {
        return m_ComponentStore.GetComponentArray<T>();
    }

    // Singleton-component sugar. Stored on a reserved hidden entity that is
    // invisible to GetActiveEntities()/GetEntityCount() and survives Clear().
    template<typename T> void SetSingleton(T value) { AddComponent<T>(m_SingletonEntity, std::move(value)); }
    template<typename T> [[nodiscard]] const T* GetSingleton() const { return GetComponent<T>(m_SingletonEntity); }
    template<typename T, typename F> void ModifySingleton(F&& fn) { Modify<T>(m_SingletonEntity, std::forward<F>(fn)); }
    [[nodiscard]] EntityId SingletonEntity() const { return m_SingletonEntity; }

    /**
     * @brief Zero-allocation iteration over entities that have all Components.
     *        If the callback accepts (EntityId, const Components&...), each
     *        queried component is passed by const reference (guaranteed non-null
     *        on a full match); otherwise the callback is invoked with (EntityId).
     *        A callback invocable both ways takes the components form.
     * @note  Each<>() (empty component pack) visits every active entity.
     * @threading const; safe on snapshots. Mutation still goes through
     *            Modify/MutateArray (the refs here are read-only).
     * @warning   In the components form, do NOT call MutateArray on a *queried*
     *            component type inside the callback: the COW clone would leave the
     *            passed const refs dangling. Mutate via the entity id + Modify, or
     *            mutate only non-queried types.
     */
    template<typename... Components, typename F>
    void Each(F&& fn) const {
        for (EntityId entity : m_EntityStore.GetActiveEntities()) {
            if constexpr (std::is_invocable_v<F, EntityId, const Components&...>) {
                const std::tuple ptrs{ GetComponent<Components>(entity)... };
                const bool all = std::apply([](auto*... p){ return (p && ...); }, ptrs);
                if (all) std::apply([&](auto*... p){ fn(entity, *p...); }, ptrs);
            } else {
                if ((HasComponent<Components>(entity) && ...)) fn(entity);
            }
        }
    }

    /**
     * @brief Single-entity in-place edit. Lambda receives a `T&` referring
     *        to the cloned-for-this-tick array slot.
     * @tparam T Component type. Must be CopyConstructible.
     * @tparam F Callable taking `T&`. Return value discarded.
     * @param e Entity that should hold T. No-op if invalid or lacking T.
     * @threading GameThread only.
     * @cow Probes Has(e) first; clones only if the entity holds the component.
     */
    template<typename T, typename F>
    void Modify(EntityId e, F&& fn) {
        const auto* readArr = m_ComponentStore.GetArray<T>();
        if (!readArr || !readArr->Has(e)) return;
        auto& writeArr = m_ComponentStore.MutateArray<T>();
        if (T* c = writeArr.Get(e)) {
            std::forward<F>(fn)(*c);
        }
    }

    /**
     * @brief Bulk-write access to the array for T. Clones once per tick.
     *        Prefer this over Modify when iterating many entities of one type.
     * @threading GameThread only.
     * @cow Triggers a clone on first call per tick.
     */
    template<typename T>
    ComponentArray<T>& MutateArray() { return m_ComponentStore.MutateArray<T>(); }

    /**
     * @brief Bulk-read access. Use for systems iterating one component type densely.
     * @return Pointer to const array, or nullptr if type unregistered.
     * @snapshot Safe through a snapshot reference; immutable for snapshot lifetime.
     */
    template<typename T>
    const ComponentArray<T>* GetArray() const { return m_ComponentStore.GetArray<T>(); }

    void Clear();

    /**
     * @brief Publishes an immutable snapshot of the world for cross-thread reads.
     *
     * Performs a shallow copy of the component-array map (refcount bumps), a deep
     * copy of EntityStore, and resets the master's per-tick dirty set. After
     * return, the next mutation on any component type clones-on-write to keep
     * the returned snapshot isolated.
     *
     * @threading GameThread only (single-writer rule).
     * @snapshot The returned ECS exposes only const accessors.
     */
    [[nodiscard]] std::shared_ptr<const ECS> CreateSnapshot();

    /**
     * @brief Resets a recycled snapshot for reuse by the pool: drops this
     *        snapshot's component-array refs (so uniquely-owned arrays free now).
     *        The reuse win is the recycled ECS shell + EntityStore vector capacity;
     *        the array map is reassigned wholesale by CreateSnapshot on reacquire.
     * @threading Runs from the pool's recycling deleter on WHATEVER thread drops
     *            the last ref. Must stay assert-free: only Cleanup() (no
     *            AssertOwnerThread). Do NOT call ClearDirty() here.
     */
    void ResetForRecycle() { m_ComponentStore.Cleanup(); }

    [[nodiscard]] EcsMemoryStats MemoryStats() const {
        size_t arrayCount = 0;
        const ArrayMemory comp = m_ComponentStore.MemoryBytes(arrayCount);
        const ArrayMemory ent  = m_EntityStore.MemoryBytes();
        return EcsMemoryStats{
            comp.Used, comp.Reserved,
            ent.Used,  ent.Reserved,
            arrayCount, m_EntityStore.GetEntityCount()
        };
    }

private:
    EntityStore m_EntityStore;
    ComponentStore m_ComponentStore;
    EntityId m_SingletonEntity = INVALID_ENTITY;
};

// Per-T extern template declarations for ECS methods. Guarded with ECS_EXPORTS
// so ecs.dll's own TU doesn't see them (provides definitions instead).
#ifndef ECS_EXPORTS
#define ECS_EXTERN_ECS_METHODS(T) \
    extern template ECS_API void ECS::AddComponent<T>(EntityId, T); \
    extern template ECS_API void ECS::RemoveComponent<T>(EntityId); \
    extern template ECS_API bool ECS::HasComponent<T>(EntityId) const; \
    extern template ECS_API const T* ECS::GetComponent<T>(EntityId) const; \
    extern template ECS_API const ComponentArray<T>* ECS::GetArray<T>() const; \
    extern template ECS_API ComponentArray<T>& ECS::MutateArray<T>();
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_EXTERN_ECS_METHODS)
#undef ECS_EXTERN_ECS_METHODS
#endif
