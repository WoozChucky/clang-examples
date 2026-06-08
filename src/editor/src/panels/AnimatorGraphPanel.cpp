#include "AnimatorGraphPanel.h"

#include <imgui.h>
#include <imgui_node_editor.h>

#include <fstream>
#include <nlohmann/json.hpp>

#include "EditorContext.h"
#include "ApplicationContext.h"
#include "ECS.h"
#include "animation/AnimatorControllerStore.h"
#include "animation/AnimationStore.h"
#include "AssetKey.h"
#include "lib.h"

namespace ed = ax::NodeEditor;

// --- deterministic, frame-stable id scheme (disjoint spaces) ---
// NodeId 0 is the node-editor's "invalid", so states are stored 1-based.
static constexpr int kAnyStateNode = 100000;
static inline ed::NodeId NodeId(int s)  { return ed::NodeId(static_cast<uintptr_t>(s + 1)); }            // states 1..N ; anyState = kAnyStateNode+1
static inline ed::PinId  InPinId(int s) { return ed::PinId(static_cast<uintptr_t>(200000 + s)); }
static inline ed::PinId  OutPinId(int s){ return ed::PinId(static_cast<uintptr_t>(300000 + s)); }
static inline ed::LinkId LinkId(int ti) { return ed::LinkId(static_cast<uintptr_t>(400000 + ti)); }

AnimatorGraphPanel::AnimatorGraphPanel() {
    ed::Config cfg;
    cfg.SettingsFile = nullptr; // we persist node positions ourselves in the .animctrl.json; don't let the lib write its own settings file
    m_Ed = ed::CreateEditor(&cfg);
}

AnimatorGraphPanel::~AnimatorGraphPanel() {
    if (m_Ed) ed::DestroyEditor(m_Ed);
}

void AnimatorGraphPanel::LoadController(uint64_t handle) {
    auto c = AnimatorControllerStore::Instance().Get(handle);
    if (!c) return;
    m_Working = *c;
    m_ControllerId = handle;
    m_ControllerKey = AnimatorControllerStore::Instance().KeyForHandle(handle);
    m_SourcePath = AnimatorControllerStore::Instance().SourcePathForHandle(handle);
    m_Layout = AnimGraphLayout{};
    if (!m_SourcePath.empty()) {
        std::ifstream f(m_SourcePath);
        if (f) { try { nlohmann::json doc; f >> doc; m_Layout = ReadLayout(doc); } catch (...) {} }
    }
    m_LayoutApplied = false;
    m_Dirty = false;
    RecomputeWarnings();
}

void AnimatorGraphPanel::RecomputeWarnings() {
    if (m_ControllerKey.empty()) {
        m_Warnings = ValidateController(m_Working);
        return;
    }
    // Strip the trailing "#animctrl" suffix to recover the model/asset key the clips live under.
    std::string assetKey = m_ControllerKey;
    const std::string suffix = "#animctrl";
    if (assetKey.size() >= suffix.size() &&
        assetKey.compare(assetKey.size() - suffix.size(), suffix.size(), suffix) == 0)
        assetKey.erase(assetKey.size() - suffix.size());

    auto resolver = [&](size_t s) -> bool {
        if (s >= m_Working.states.size()) return false;
        const std::string clipKey = m_Working.states[s].clipKey;
        const uint64_t h = AssetKeyHash(assetKey + "#anim/" + clipKey);
        return AnimationStore::Instance().Get(h) != nullptr;
    };
    m_Warnings = ValidateController(m_Working, resolver);
}

void AnimatorGraphPanel::Draw(const EditorContext& ctx, bool* open) {
    if (!*open) return;
    if (!ImGui::Begin("Animator Graph", open)) { ImGui::End(); return; }

    // --- resolve the selected entity + its live AnimatorComponent (for auto-load + highlight) ---
    EntityId sel = INVALID_ENTITY;
    if (ctx.App) sel = ctx.App->SelectedEntity.load(std::memory_order_relaxed);
    const AnimatorComponent* liveAnim = nullptr;
    if (sel != INVALID_ENTITY && ctx.WorldSnapshot)
        liveAnim = ctx.WorldSnapshot->GetComponent<AnimatorComponent>(sel);

    // Auto-select the selected entity's controller only when nothing is loaded yet (don't fight a manual pick).
    if (m_ControllerId == 0 && liveAnim && liveAnim->ControllerId != 0)
        LoadController(liveAnim->ControllerId);

    // --- controller picker ---
    const char* currentLabel = m_ControllerKey.empty() ? "(none)" : m_ControllerKey.c_str();
    if (ImGui::BeginCombo("Controller", currentLabel)) {
        const auto list = AnimatorControllerStore::Instance().GetAssetList();
        if (list.empty()) ImGui::TextDisabled("No controllers loaded");
        for (const auto& [handle, key] : list) {
            const bool isSel = (handle == m_ControllerId);
            const char* label = key.empty() ? "(none)" : key.c_str();
            if (ImGui::Selectable(label, isSel) && handle != m_ControllerId)
                LoadController(handle);
            if (isSel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // --- toolbar ---
    ImGui::BeginDisabled(!m_Dirty);
    if (ImGui::Button("Save")) SM_WARN("AnimatorGraphPanel: Save not implemented yet");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reload from disk")) SM_WARN("AnimatorGraphPanel: Reload from disk not implemented yet");
    if (!m_Warnings.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "warning %d", (int)m_Warnings.size());
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            for (const auto& w : m_Warnings) ImGui::TextUnformatted(w.c_str());
            ImGui::EndTooltip();
        }
    }

    ImGui::Separator();

    // --- live highlight cursor: only valid when the selected entity uses THIS controller ---
    int   liveCurrent = -1;
    int   liveFrom    = -1;
    float liveW       = 1.0f;
    if (liveAnim && liveAnim->ControllerId == m_ControllerId && m_ControllerId != 0) {
        liveCurrent = liveAnim->CurrentState;
        liveFrom    = liveAnim->FromState;
        liveW = liveAnim->TransitionDur > 0.0f ? liveAnim->TransitionElapsed / liveAnim->TransitionDur : 1.0f;
    }

    // does any transition originate from anyState?
    bool hasAnyState = false;
    for (const auto& t : m_Working.transitions) if (t.from == "*") { hasAnyState = true; break; }

    // --- canvas ---
    ed::SetCurrentEditor(m_Ed);
    ed::Begin("AnimatorGraphCanvas", ImVec2(0, 0));

    const ImVec4 kActiveBorder(0.20f, 1.0f, 0.30f, 1.0f); // green = active state

    // state nodes
    for (int s = 0; s < (int)m_Working.states.size(); ++s) {
        const AnimState& st = m_Working.states[s];
        if (!m_LayoutApplied) {
            auto it = m_Layout.nodes.find(st.name);
            if (it != m_Layout.nodes.end())
                ed::SetNodePosition(NodeId(s), ImVec2(it->second[0], it->second[1]));
            else
                ed::SetNodePosition(NodeId(s), ImVec2(40.0f + s * 180.0f, 40.0f));
        }

        const bool active = (s == liveCurrent);
        if (active) ed::PushStyleColor(ed::StyleColor_NodeBorder, kActiveBorder);

        ed::BeginNode(NodeId(s));
        ImGui::Text("%s%s", st.name.c_str(), s == 0 ? " (entry)" : "");
        ImGui::TextDisabled("%s", st.clipKey.empty() ? "(no clip)" : st.clipKey.c_str());
        if (st.cyclic || st.loop) ImGui::TextDisabled("%s%s", st.cyclic ? "[cyclic]" : "", st.loop ? "[loop]" : "");
        if (active) ImGui::TextColored(kActiveBorder, "<- ACTIVE");

        ed::BeginPin(InPinId(s), ed::PinKind::Input);
        ImGui::Text("in");
        ed::EndPin();
        ImGui::SameLine();
        ed::BeginPin(OutPinId(s), ed::PinKind::Output);
        ImGui::Text("out");
        ed::EndPin();
        ed::EndNode();

        if (active) ed::PopStyleColor();
    }

    // anyState node (only if referenced)
    if (hasAnyState) {
        if (!m_LayoutApplied) {
            auto it = m_Layout.nodes.find("__any__");
            if (it != m_Layout.nodes.end())
                ed::SetNodePosition(NodeId(kAnyStateNode), ImVec2(it->second[0], it->second[1]));
            else
                ed::SetNodePosition(NodeId(kAnyStateNode), ImVec2(40.0f, 220.0f));
        }
        ed::BeginNode(NodeId(kAnyStateNode));
        ImGui::Text("Any State");
        ed::BeginPin(OutPinId(kAnyStateNode), ed::PinKind::Output);
        ImGui::Text("out");
        ed::EndPin();
        ed::EndNode();
    }

    m_LayoutApplied = true;

    // links
    static bool s_WarnedMissing = false;
    for (int ti = 0; ti < (int)m_Working.transitions.size(); ++ti) {
        const AnimTransition& t = m_Working.transitions[ti];
        const int toIdx = FindState(m_Working, t.to);
        if (toIdx < 0) {
            if (!s_WarnedMissing) { SM_WARN("AnimatorGraphPanel: transition references missing state(s); skipping link"); s_WarnedMissing = true; }
            continue;
        }
        ed::PinId srcPin;
        if (t.from == "*") {
            srcPin = OutPinId(kAnyStateNode);
        } else {
            const int fromIdx = FindState(m_Working, t.from);
            if (fromIdx < 0) {
                if (!s_WarnedMissing) { SM_WARN("AnimatorGraphPanel: transition references missing state(s); skipping link"); s_WarnedMissing = true; }
                continue;
            }
            srcPin = OutPinId(fromIdx);
        }
        const ed::PinId dstPin = InPinId(toIdx);

        // Approximate active-transition highlight: a transition whose endpoints match the live cursor.
        const bool activeLink = (liveFrom >= 0 && liveFrom < (int)m_Working.states.size() &&
                                 liveCurrent >= 0 && liveCurrent < (int)m_Working.states.size() &&
                                 t.from == m_Working.states[liveFrom].name &&
                                 t.to   == m_Working.states[liveCurrent].name);
        if (activeLink)
            ed::Link(LinkId(ti), srcPin, dstPin, ImVec4(0.20f, 1.0f, 0.30f, 1.0f), 3.0f);
        else
            ed::Link(LinkId(ti), srcPin, dstPin);
    }

    ed::End();
    ed::SetCurrentEditor(nullptr);

    if (liveFrom >= 0)
        ImGui::Text("Active transition weight: %.2f", liveW);

    ImGui::End();
}
