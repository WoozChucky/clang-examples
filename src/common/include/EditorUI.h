#pragma once
#include <nlohmann/json.hpp>

// ImGui-free widget bridge: the editor implements these over Dear ImGui; a game component's
// editor hook (in Game.dll, which links no ImGui) calls them to draw/edit fields of a JSON
// working copy. Keys are the component's to_json field names. Each editing widget returns true
// if the value changed this frame; a missing/wrong-typed key is a safe no-op returning false.
// The editor passes an EditorUI into the hook; Game.dll only calls through the pointers.
struct EditorUI {
    bool (*DragFloat) (nlohmann::json& obj, const char* key, float speed);
    bool (*DragFloat2)(nlohmann::json& obj, const char* key, float speed);
    bool (*DragFloat3)(nlohmann::json& obj, const char* key, float speed);
    bool (*DragFloat4)(nlohmann::json& obj, const char* key, float speed);
    bool (*InputInt)  (nlohmann::json& obj, const char* key);
    bool (*Checkbox)  (nlohmann::json& obj, const char* key);
    bool (*Combo)     (nlohmann::json& obj, const char* key, const char* const* labels, int count);
    bool (*ColorEdit3)(nlohmann::json& obj, const char* key);
    bool (*ColorEdit4)(nlohmann::json& obj, const char* key);
    void (*Text)      (const char* text);
    void (*Separator) ();
    void (*SameLine)  ();
    bool (*Button)    (const char* label);
};
