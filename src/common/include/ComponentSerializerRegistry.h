#pragma once
#include <string>
#include <vector>
#include <cstddef>   // std::byte
#include <cstring>   // std::memcpy
#include <type_traits>

#include <nlohmann/json.hpp>
#include "ECS.h"   // ECS, EntityId, ECS_API

// Registry of per-component (de)serializers keyed by a stable string name (the world.json
// key). Built-in component types register once in ecs.dll; Game.dll registers its own types
// additively — header-instantiable ECS<T> methods (boundary Piece 1) make the <T> lambdas
// compile in whatever module registers the type. A single exported instance is shared by all
// modules.
//
struct EditorUI; // defined in EditorUI.h (editor implements it over ImGui); only a fn-ptr param here

// True when nlohmann can round-trip T via ADL to_json/from_json (i.e. T has a serializer).
// Used to decide whether Register<T> installs the json save/load path.
template <class T>
concept JsonSerializable = requires(nlohmann::json& j, const T& cv, T& v) {
    { j = cv };
    { v = j.template get<T>() };
};

// Entry function pointers are CAPTURELESS lambdas compiled in the registering module:
//   has(world, e)       -> does entity e have this component?
//   save(world, e, out) -> out = the component's json value (caller keys it by `name`)
//   load(world, e, in)  -> AddComponent<T>(e, in.get<T>())
struct ComponentSerializerEntry {
    std::string name;
    bool (*has)(const ECS&, EntityId);
    void (*save)(const ECS&, EntityId, nlohmann::json&);
    void (*load)(ECS&, EntityId, const nlohmann::json&);
    void (*addDefault)(ECS&, EntityId);   // AddComponent<T>(e, T{})
    void (*remove)(ECS&, EntityId);       // RemoveComponent<T>(e)
    bool builtin;                         // true for ecs.dll's built-ins; false for game types
    // Optional custom editor renderer (game-provided, ImGui-free via the EditorUI bridge).
    // Null => the inspector uses the generic JSON-tree editor. Set via RegisterEditorHook.
    bool (*editorDraw)(const EditorUI&, nlohmann::json&) = nullptr;
    // Reload-preservation (byte path), distinct from the json save/load used for world.json disk
    // persistence. Auto-installed for trivially-copyable T. Null => no byte path for this type.
    void (*reloadExtract)(const ECS&, EntityId, std::vector<std::byte>&) = nullptr; // memcpy T out
    void (*reloadIngest)(ECS&, EntityId, const std::vector<std::byte>&)  = nullptr; // memcpy T in + AddComponent
};

class ComponentSerializerRegistry {
public:
    // Upsert: re-registering an existing name REPLACES its function pointers. Required for
    // Game.dll hot-reload — a reloaded DLL re-registers its types so the registry never holds
    // dangling pointers into the unloaded module.
    template <class T>
    void Register(const std::string& name, bool builtin = false) {
        ComponentSerializerEntry e{};
        e.name       = name;
        e.has        = [](const ECS& w, EntityId en)                      { return w.HasComponent<T>(en); };
        e.addDefault = [](ECS& w, EntityId en)                           { w.AddComponent<T>(en, T{}); };
        e.remove     = [](ECS& w, EntityId en)                           { w.RemoveComponent<T>(en); };
        e.builtin    = builtin;

        if constexpr (JsonSerializable<T>) {
            e.save = [](const ECS& w, EntityId en, nlohmann::json& out) { out = *w.GetComponent<T>(en); };
            e.load = [](ECS& w, EntityId en, const nlohmann::json& in)  { w.AddComponent<T>(en, in.template get<T>()); };
        }
        if constexpr (std::is_trivially_copyable_v<T>) {
            e.reloadExtract = [](const ECS& w, EntityId en, std::vector<std::byte>& out) {
                const T* p = w.GetComponent<T>(en);
                if (!p) { out.clear(); return; }
                out.resize(sizeof(T));
                std::memcpy(out.data(), p, sizeof(T));
            };
            e.reloadIngest = [](ECS& w, EntityId en, const std::vector<std::byte>& in) {
                if (in.size() != sizeof(T)) return;
                T t{};
                std::memcpy(&t, in.data(), sizeof(T));
                w.AddComponent<T>(en, std::move(t));
            };
        }

        for (auto& existing : m_Entries) {
            if (existing.name == name) { existing = e; return; }
        }
        m_Entries.push_back(std::move(e));
    }

    [[nodiscard]] const std::vector<ComponentSerializerEntry>& Entries() const { return m_Entries; }

    [[nodiscard]] const ComponentSerializerEntry* Find(const std::string& name) const {
        for (const auto& e : m_Entries) if (e.name == name) return &e;
        return nullptr;
    }

    // Attach an optional custom editor renderer to an already-registered component (by name).
    // No-op (warns) if the name isn't registered. Defined in ComponentSerializers.cpp.
    ECS_API void RegisterEditorHook(const std::string& name, bool (*draw)(const EditorUI&, nlohmann::json&));

private:
    std::vector<ComponentSerializerEntry> m_Entries;
};

// Single process-wide instance (defined + exported from ecs.dll). Lazily registers all
// built-in persisted component serializers on first use.
ECS_API ComponentSerializerRegistry& SerializerRegistry();

// Generic per-entity (de)serialization used by WorldManager (and tests). Save writes each
// present component under its registered name into `jEntity`. Load reads every key (skipping
// "EntityId") through the registry, warning on an unknown component key.
ECS_API void SaveEntityComponents(const ECS& world, EntityId e, nlohmann::json& jEntity);
ECS_API void LoadEntityComponents(ECS& world, EntityId e, const nlohmann::json& jEntity);
