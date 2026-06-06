#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include "ECS.h"

struct EditorContext;

// Renders any REGISTERED component as an editable JSON tree (for game-defined types the
// editor cannot name; built-ins keep their bespoke IComponentEditor). Reads the component
// from the snapshot via the serializer registry, edits a working JSON copy, and commits a
// ModifyComponentJson command. One instance is reused for all generic components; it keys
// its working copy by (entity, component-name) so switching selection re-syncs.
class GenericComponentEditor {
public:
    void Draw(const EditorContext& ctx, EntityId entity, const std::string& name);

private:
    EntityId       m_Entity = INVALID_ENTITY;
    std::string    m_Name;
    nlohmann::json m_Edit;
    bool           m_Modified = false;

    static bool DrawJsonValue(const char* label, nlohmann::json& value);
};
