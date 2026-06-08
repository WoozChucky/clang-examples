#pragma once
#include <string>
#include <unordered_map>
#include <array>
#include <nlohmann/json.hpp>

// Editor-only graph layout persisted in the .animctrl.json "editorLayout" block. Keyed by state name;
// the anyState node uses the reserved key "__any__". Runtime ignores this block.
struct AnimGraphLayout {
    std::unordered_map<std::string, std::array<float,2>> nodes; // name -> {x,y}
    std::array<float,2> pan{0.0f, 0.0f};
    float zoom = 1.0f;
};

inline AnimGraphLayout ReadLayout(const nlohmann::json& doc) {
    AnimGraphLayout L;
    if (!doc.contains("editorLayout")) return L;
    const auto& e = doc.at("editorLayout");
    if (e.contains("nodes"))
        for (auto it = e.at("nodes").begin(); it != e.at("nodes").end(); ++it)
            if (it.value().is_array() && it.value().size() == 2)
                L.nodes[it.key()] = { it.value()[0].get<float>(), it.value()[1].get<float>() };
    if (e.contains("pan") && e.at("pan").is_array() && e.at("pan").size() == 2)
        L.pan = { e.at("pan")[0].get<float>(), e.at("pan")[1].get<float>() };
    L.zoom = e.value("zoom", 1.0f);
    return L;
}

inline void WriteLayout(nlohmann::json& doc, const AnimGraphLayout& L) {
    nlohmann::json nodes = nlohmann::json::object();
    for (const auto& [name, xy] : L.nodes) nodes[name] = { xy[0], xy[1] };
    doc["editorLayout"] = { {"nodes", nodes}, {"pan", { L.pan[0], L.pan[1] }}, {"zoom", L.zoom} };
}
