#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cmath>
#include <functional>
#include <nlohmann/json.hpp>

// Data-driven animation state machine (Unity-Animator-mapped). Pure data + pure helpers; no engine
// or ECS dependency, so the evaluator, JSON loader, and unit tests all share it. See
// docs/superpowers/specs/2026-06-08-anim-statemachine-design.md.

enum class AnimParamType { Float, Bool, Trigger };
// EvalCondition always applies the numeric `op` regardless of param type (no special-casing of
// Bool/Trigger). `Equal` does an exact float == compare with no epsilon, so it is meant for discrete
// params only (Bool/Trigger/integer-valued Float); using it on a continuous, velocity-driven Float
// (e.g. `speed`) will almost never fire because of floating-point noise.
enum class AnimCondOp { Greater, Less, GreaterEqual, LessEqual, Equal };

struct AnimParam     { std::string name; AnimParamType type = AnimParamType::Float; };
// clipKey = BARE clip name. `cyclic`: when both endpoints of a transition are cyclic -> dual-cursor
// phase-synced crossfade (gait-matching for locomotion); else snapshot crossfade. `loop`: clip-time
// wrap for NON-cyclic states (cyclic states always loop via Phase) -- true wraps StateTime so a
// non-cyclic state keeps playing (e.g. Idle), false clamps so a one-shot holds its last frame (e.g.
// Hit). Default true.
struct AnimState     { std::string name; std::string clipKey; bool cyclic = false; bool loop = true; };
struct AnimCondition { std::string paramName; AnimCondOp op = AnimCondOp::Greater; float value = 0.0f; };
struct AnimTransition {
    std::string from;                       // "*" = anyState
    std::string to;
    float duration = 0.2f;                  // seconds
    std::vector<AnimCondition> conditions;  // ALL must hold (AND)
};
struct AnimatorController {
    std::string name;
    std::vector<AnimParam>      params;
    std::vector<AnimState>      states;       // index 0 = entry/default state
    std::vector<AnimTransition> transitions;
    std::vector<uint64_t>       stateClipIds; // resolved at load (parallel to states); 0 = unresolved
};

// --- pure helpers (unit-tested) ---

inline bool EvalCondition(AnimCondOp op, float paramValue, float threshold) {
    switch (op) {
        case AnimCondOp::Greater:      return paramValue >  threshold;
        case AnimCondOp::Less:         return paramValue <  threshold;
        case AnimCondOp::GreaterEqual: return paramValue >= threshold;
        case AnimCondOp::LessEqual:    return paramValue <= threshold;
        case AnimCondOp::Equal:        return paramValue == threshold;
    }
    return false;
}

inline float WrapPhase01(float phase) {
    if (phase >= 0.0f && phase < 1.0f) return phase;
    phase = std::fmod(phase, 1.0f);
    if (phase < 0.0f) phase += 1.0f;
    return phase;
}

inline float PhaseToTime(float phase, float duration) { return WrapPhase01(phase) * duration; }

// Find the index in controller.transitions of the first transition that should fire from
// `currentState`, given a parameter lookup. anyState ("*") transitions are evaluated first (in
// declared order), then the current state's outgoing (declared order). -1 = none.
inline int SelectTransition(const AnimatorController& c, int currentState,
                            const std::function<float(const std::string&)>& param) {
    auto allHold = [&](const AnimTransition& t) {
        for (const auto& cond : t.conditions)
            if (!EvalCondition(cond.op, param(cond.paramName), cond.value)) return false;
        return true; // all conditions held (an empty condition list trivially fires)
    };
    const std::string current =
        (currentState >= 0 && currentState < (int)c.states.size()) ? c.states[currentState].name : std::string();
    // Pass 1: anyState.
    for (size_t i = 0; i < c.transitions.size(); ++i)
        if (c.transitions[i].from == "*" && c.transitions[i].to != current && allHold(c.transitions[i]))
            return (int)i;
    // Pass 2: outgoing from current.
    for (size_t i = 0; i < c.transitions.size(); ++i)
        if (c.transitions[i].from == current && allHold(c.transitions[i]))
            return (int)i;
    return -1;
}

// Index of a state by name; -1 if not found.
inline int FindState(const AnimatorController& c, const std::string& name) {
    for (size_t i = 0; i < c.states.size(); ++i) if (c.states[i].name == name) return (int)i;
    return -1;
}

// --- JSON (de)serialization ---

NLOHMANN_JSON_SERIALIZE_ENUM(AnimParamType, {
    {AnimParamType::Float, "Float"}, {AnimParamType::Bool, "Bool"}, {AnimParamType::Trigger, "Trigger"},
})
NLOHMANN_JSON_SERIALIZE_ENUM(AnimCondOp, {
    {AnimCondOp::Greater, "Greater"}, {AnimCondOp::Less, "Less"}, {AnimCondOp::GreaterEqual, "GreaterEqual"},
    {AnimCondOp::LessEqual, "LessEqual"}, {AnimCondOp::Equal, "Equal"},
})
inline void to_json(nlohmann::json& j, const AnimParam& p) { j = {{"name", p.name}, {"type", p.type}}; }
inline void from_json(const nlohmann::json& j, AnimParam& p) {
    j.at("name").get_to(p.name); p.type = j.value("type", AnimParamType::Float);
}
inline void to_json(nlohmann::json& j, const AnimState& s) { j = {{"name", s.name}, {"clipKey", s.clipKey}, {"cyclic", s.cyclic}, {"loop", s.loop}}; }
inline void from_json(const nlohmann::json& j, AnimState& s) {
    j.at("name").get_to(s.name); s.clipKey = j.value("clipKey", std::string()); s.cyclic = j.value("cyclic", false);
    s.loop = j.value("loop", true);
}
inline void to_json(nlohmann::json& j, const AnimCondition& c) { j = {{"paramName", c.paramName}, {"op", c.op}, {"value", c.value}}; }
inline void from_json(const nlohmann::json& j, AnimCondition& c) {
    j.at("paramName").get_to(c.paramName); c.op = j.value("op", AnimCondOp::Greater); c.value = j.value("value", 0.0f);
}
inline void to_json(nlohmann::json& j, const AnimTransition& t) {
    j = {{"from", t.from}, {"to", t.to}, {"duration", t.duration}, {"conditions", t.conditions}};
}
inline void from_json(const nlohmann::json& j, AnimTransition& t) {
    j.at("from").get_to(t.from); j.at("to").get_to(t.to);
    t.duration = j.value("duration", 0.2f);
    t.conditions = j.value("conditions", std::vector<AnimCondition>{});
}
inline void to_json(nlohmann::json& j, const AnimatorController& c) {
    j = {{"name", c.name}, {"params", c.params}, {"states", c.states}, {"transitions", c.transitions}};
}
inline void from_json(const nlohmann::json& j, AnimatorController& c) {
    c.name = j.value("name", std::string());
    c.params      = j.value("params",      std::vector<AnimParam>{});
    c.states      = j.value("states",      std::vector<AnimState>{});
    c.transitions = j.value("transitions", std::vector<AnimTransition>{});
    // stateClipIds resolved at load time (engine), not from JSON.
}
