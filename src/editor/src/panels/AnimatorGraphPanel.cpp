#include "AnimatorGraphPanel.h"

#include <imgui.h>
#include <imgui_node_editor.h>

#include <algorithm>
#include <cstring>
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
// Node/pin ids derive from a STABLE per-state UID (m_StateUids), NOT the vector index, so positions
// and selection stay glued to the right state across insert/delete/reorder. The anyState node uses
// the reserved UID kAnyStateNode. UID 0 is never assigned (node-editor treats id 0 as "invalid").
static constexpr uint32_t kAnyStateNode = 100000;
static inline ed::NodeId NodeId(uint32_t uid)  { return ed::NodeId(static_cast<uintptr_t>(uid)); }
static inline ed::PinId  InPinId(uint32_t uid) { return ed::PinId(static_cast<uintptr_t>(200000u + uid)); }
static inline ed::PinId  OutPinId(uint32_t uid){ return ed::PinId(static_cast<uintptr_t>(300000u + uid)); }
static inline ed::LinkId LinkId(int ti)        { return ed::LinkId(static_cast<uintptr_t>(400000 + ti)); }

// Decode a pin id back to its owning UID (and which side). Returns the UID; kind set via out-param.
// Returns 0 if the id is in neither pin space.
static uint32_t DecodePin(const ed::PinId& pin, bool& isInput) {
    const uintptr_t v = pin.Get();
    if (v >= 300000u && v < 400000u) { isInput = false; return static_cast<uint32_t>(v - 300000u); }
    if (v >= 200000u && v < 300000u) { isInput = true;  return static_cast<uint32_t>(v - 200000u); }
    return 0;
}

AnimatorGraphPanel::AnimatorGraphPanel() {
    ed::Config cfg;
    cfg.SettingsFile = nullptr; // we persist node positions ourselves in the .animctrl.json; don't let the lib write its own settings file
    m_Ed = ed::CreateEditor(&cfg);
}

AnimatorGraphPanel::~AnimatorGraphPanel() {
    if (m_Ed) ed::DestroyEditor(m_Ed);
}

int AnimatorGraphPanel::StateIndexForUid(uint32_t uid) const {
    for (int i = 0; i < (int)m_StateUids.size(); ++i)
        if (m_StateUids[i] == uid) return i;
    return -1;
}

std::vector<std::string> AnimatorGraphPanel::AvailableClips() const {
    std::vector<std::string> out;
    if (m_ControllerKey.empty()) return out;
    std::string assetKey = m_ControllerKey;
    const std::string suffix = "#animctrl";
    if (assetKey.size() >= suffix.size() &&
        assetKey.compare(assetKey.size() - suffix.size(), suffix.size(), suffix) == 0)
        assetKey.erase(assetKey.size() - suffix.size());
    const std::string prefix = assetKey + "#anim/";
    for (const auto& [handle, key] : AnimationStore::Instance().GetAssetList()) {
        if (key.size() > prefix.size() && key.compare(0, prefix.size(), prefix) == 0)
            out.push_back(key.substr(prefix.size())); // bare clip name
    }
    std::sort(out.begin(), out.end());
    return out;
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
    // Rebuild stable canvas UIDs: one fresh uid per state, parallel to m_Working.states.
    m_StateUids.assign(m_Working.states.size(), 0);
    for (auto& u : m_StateUids) u = m_NextUid++;
    m_ShowAnyState = false;
    for (const auto& t : m_Working.transitions) if (t.from == "*") { m_ShowAnyState = true; break; }
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
    ImGui::SameLine();
    const bool haveController = (m_ControllerId != 0);
    ImGui::BeginDisabled(!haveController);
    if (ImGui::Button("Add State")) {
        // unique name
        int n = (int)m_Working.states.size();
        std::string name;
        do { name = "State" + std::to_string(n++); } while (FindState(m_Working, name) >= 0);
        m_Working.states.push_back(AnimState{ name, "", false, true });
        m_StateUids.push_back(m_NextUid++);
        // cascade placement near where existing nodes are
        const float x = 60.0f + (float)(m_Working.states.size() % 6) * 40.0f;
        const float y = 60.0f + (float)(m_Working.states.size() % 6) * 40.0f;
        m_Layout.nodes[name] = { x, y };
        ed::SetCurrentEditor(m_Ed);
        ed::SetNodePosition(NodeId(m_StateUids.back()), ImVec2(x, y));
        ed::SetCurrentEditor(nullptr);
        MarkEdited();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Any-State")) m_ShowAnyState = true;
    ImGui::EndDisabled();

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
    for (const auto& t : m_Working.transitions) if (t.from == "*") { m_ShowAnyState = true; break; }
    const bool hasAnyState = m_ShowAnyState;

    const std::vector<std::string> clips = AvailableClips();

    // Split into canvas (left) + inspector (right) so the selected-link / params UI lives off-canvas.
    const float kInspectorW = 300.0f;
    ImGui::BeginChild("##animgraph_canvas", ImVec2(-kInspectorW, 0), false);
    ed::SetCurrentEditor(m_Ed);
    ed::Begin("AnimatorGraphCanvas", ImVec2(0, 0));

    const ImVec4 kActiveBorder(0.20f, 1.0f, 0.30f, 1.0f); // green = active state

    // Deferred mutations from in-node widgets (apply AFTER the node loop to avoid invalidating
    // m_Working.states while iterating / while node ids are live this frame).
    int        renameIdx = -1; std::string renameOld, renameNew;
    int        setEntryIdx = -1;

    // state nodes
    for (int s = 0; s < (int)m_Working.states.size(); ++s) {
        AnimState& st = m_Working.states[s];
        const uint32_t uid = m_StateUids[s];
        if (!m_LayoutApplied) {
            auto it = m_Layout.nodes.find(st.name);
            if (it != m_Layout.nodes.end())
                ed::SetNodePosition(NodeId(uid), ImVec2(it->second[0], it->second[1]));
            else
                ed::SetNodePosition(NodeId(uid), ImVec2(40.0f + s * 180.0f, 40.0f));
        }

        const bool active = (s == liveCurrent);
        if (active) ed::PushStyleColor(ed::StyleColor_NodeBorder, kActiveBorder);

        ed::BeginNode(NodeId(uid));
        ImGui::PushID((int)uid);

        if (s == 0) ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "(entry)");

        // Name editor: a per-uid static buffer seeded from the current name when not active.
        char nameBuf[64];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", st.name.c_str());
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            std::string nn = nameBuf;
            if (!nn.empty() && nn != st.name) { renameIdx = s; renameOld = st.name; renameNew = nn; }
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            std::string nn = nameBuf;
            if (!nn.empty() && nn != st.name) { renameIdx = s; renameOld = st.name; renameNew = nn; }
        }

        // Clip dropdown.
        ImGui::SetNextItemWidth(150.0f);
        const char* clipLabel = st.clipKey.empty() ? "(none)" : st.clipKey.c_str();
        if (ImGui::BeginCombo("##clip", clipLabel)) {
            if (ImGui::Selectable("(none)", st.clipKey.empty())) { st.clipKey.clear(); MarkEdited(); }
            for (const auto& cn : clips) {
                const bool csel = (cn == st.clipKey);
                if (ImGui::Selectable(cn.c_str(), csel) && cn != st.clipKey) { st.clipKey = cn; MarkEdited(); }
                if (csel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (ImGui::Checkbox("cyclic", &st.cyclic)) MarkEdited();
        ImGui::SameLine();
        if (ImGui::Checkbox("loop", &st.loop)) MarkEdited();

        if (s != 0 && ImGui::SmallButton("Set as entry")) setEntryIdx = s;

        if (active) ImGui::TextColored(kActiveBorder, "<- ACTIVE");

        ed::BeginPin(InPinId(uid), ed::PinKind::Input);
        ImGui::Text("in");
        ed::EndPin();
        ImGui::SameLine();
        ed::BeginPin(OutPinId(uid), ed::PinKind::Output);
        ImGui::Text("out");
        ed::EndPin();

        ImGui::PopID();
        ed::EndNode();

        if (active) ed::PopStyleColor();
    }

    // anyState node (shown if referenced OR user requested it)
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
            srcPin = OutPinId(m_StateUids[fromIdx]);
        }
        const ed::PinId dstPin = InPinId(m_StateUids[toIdx]);

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

    // --- create new links (transitions) ---
    if (ed::BeginCreate()) {
        ed::PinId startPin, endPin;
        if (ed::QueryNewLink(&startPin, &endPin)) {
            if (startPin && endPin) {
                bool aIn = false, bIn = false;
                uint32_t aUid = DecodePin(startPin, aIn);
                uint32_t bUid = DecodePin(endPin, bIn);
                // Normalize so 'start' is the OUTPUT and 'end' is the INPUT.
                if (aIn && !bIn) { std::swap(aUid, bUid); std::swap(aIn, bIn); }
                const bool valid = (!aIn && bIn);                 // output -> input only
                if (valid && ed::AcceptNewItem()) {
                    std::string fromName;
                    if (aUid == kAnyStateNode) fromName = "*";
                    else { int fi = StateIndexForUid(aUid); if (fi >= 0) fromName = m_Working.states[fi].name; }
                    int ti = StateIndexForUid(bUid);
                    if (!fromName.empty() && ti >= 0) {
                        m_Working.transitions.push_back(AnimTransition{ fromName, m_Working.states[ti].name, 0.2f, {} });
                        if (fromName == "*") m_ShowAnyState = true;
                        MarkEdited();
                    }
                } else if (!valid) {
                    ed::RejectNewItem();
                }
            }
        }
    }
    ed::EndCreate();

    // --- delete nodes / links ---
    if (ed::BeginDelete()) {
        ed::NodeId delNode;
        while (ed::QueryDeletedNode(&delNode)) {
            const uint32_t uid = static_cast<uint32_t>(delNode.Get());
            if (uid == kAnyStateNode) { ed::RejectDeletedItem(); continue; } // anyState isn't a real state
            const int idx = StateIndexForUid(uid);
            if (idx >= 0 && ed::AcceptDeletedItem()) {
                const std::string name = m_Working.states[idx].name;
                // drop transitions referencing this state
                auto& tr = m_Working.transitions;
                tr.erase(std::remove_if(tr.begin(), tr.end(),
                         [&](const AnimTransition& t){ return t.from == name || t.to == name; }), tr.end());
                m_Working.states.erase(m_Working.states.begin() + idx);
                m_StateUids.erase(m_StateUids.begin() + idx);
                m_Layout.nodes.erase(name);
                MarkEdited();
            } else if (idx < 0) {
                ed::RejectDeletedItem();
            }
        }
        ed::LinkId delLink;
        while (ed::QueryDeletedLink(&delLink)) {
            const uintptr_t v = delLink.Get();
            if (v >= 400000u) {
                const int ti = (int)(v - 400000u);
                if (ti >= 0 && ti < (int)m_Working.transitions.size() && ed::AcceptDeletedItem()) {
                    m_Working.transitions.erase(m_Working.transitions.begin() + ti);
                    MarkEdited();
                } else {
                    ed::RejectDeletedItem();
                }
            } else {
                ed::RejectDeletedItem();
            }
        }
    }
    ed::EndDelete();

    // capture selected link (for the inspector) before End()
    ed::LinkId selLinks[1];
    const int selLinkCount = ed::GetSelectedLinks(selLinks, 1);

    ed::End();
    ed::SetCurrentEditor(nullptr);

    if (liveFrom >= 0)
        ImGui::Text("Active transition weight: %.2f", liveW);

    ImGui::EndChild(); // canvas

    // --- apply deferred node-widget mutations ---
    if (renameIdx >= 0 && renameIdx < (int)m_Working.states.size()) {
        // move layout entry old->new, then rewrite states + transitions
        auto it = m_Layout.nodes.find(renameOld);
        std::array<float,2> xy = (it != m_Layout.nodes.end()) ? it->second : std::array<float,2>{0.0f, 0.0f};
        if (it != m_Layout.nodes.end()) m_Layout.nodes.erase(it);
        m_Layout.nodes[renameNew] = xy;
        RenameState(m_Working, renameOld, renameNew);
        MarkEdited();
    }
    if (setEntryIdx > 0 && setEntryIdx < (int)m_Working.states.size()) {
        // rotate state setEntryIdx to index 0; keep its uid aligned.
        AnimState st = m_Working.states[setEntryIdx];
        uint32_t  u  = m_StateUids[setEntryIdx];
        m_Working.states.erase(m_Working.states.begin() + setEntryIdx);
        m_StateUids.erase(m_StateUids.begin() + setEntryIdx);
        m_Working.states.insert(m_Working.states.begin(), st);
        m_StateUids.insert(m_StateUids.begin(), u);
        MarkEdited();
    }

    // --- inspector (right) ---
    ImGui::SameLine();
    ImGui::BeginChild("##animgraph_inspector", ImVec2(0, 0), true);

    // Parameters
    if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
        int removeParam = -1;
        for (int p = 0; p < (int)m_Working.params.size(); ++p) {
            ImGui::PushID(p);
            AnimParam& pa = m_Working.params[p];
            char buf[64]; std::snprintf(buf, sizeof(buf), "%s", pa.name.c_str());
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::InputText("##pname", buf, sizeof(buf))) { pa.name = buf; MarkEdited(); }
            ImGui::SameLine();
            const char* typeNames[] = { "Float", "Bool", "Trigger" };
            int ti = (int)pa.type;
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::Combo("##ptype", &ti, typeNames, 3)) { pa.type = (AnimParamType)ti; MarkEdited(); }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) removeParam = p;
            ImGui::PopID();
        }
        if (removeParam >= 0) { m_Working.params.erase(m_Working.params.begin() + removeParam); MarkEdited(); }
        if (ImGui::SmallButton("+ Add param")) {
            m_Working.params.push_back(AnimParam{ "param" + std::to_string(m_Working.params.size()), AnimParamType::Float });
            MarkEdited();
        }
    }

    ImGui::Separator();

    // Selected-transition inspector
    if (selLinkCount == 1) {
        const uintptr_t v = selLinks[0].Get();
        if (v >= 400000u) {
            const int ti = (int)(v - 400000u);
            if (ti >= 0 && ti < (int)m_Working.transitions.size()) {
                AnimTransition& t = m_Working.transitions[ti];
                ImGui::Text("Transition: %s -> %s", t.from.c_str(), t.to.c_str());

                // priority reorder
                ImGui::BeginDisabled(ti == 0);
                if (ImGui::SmallButton("Up")) { std::swap(m_Working.transitions[ti], m_Working.transitions[ti - 1]); MarkEdited(); }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(ti >= (int)m_Working.transitions.size() - 1);
                if (ImGui::SmallButton("Down")) { std::swap(m_Working.transitions[ti], m_Working.transitions[ti + 1]); MarkEdited(); }
                ImGui::EndDisabled();

                if (ImGui::DragFloat("duration", &t.duration, 0.01f, 0.0f, 10.0f)) MarkEdited();

                ImGui::SeparatorText("Conditions");
                const char* opNames[] = { "Greater", "Less", "GreaterEqual", "LessEqual", "Equal" };
                int removeCond = -1;
                for (int ci = 0; ci < (int)t.conditions.size(); ++ci) {
                    ImGui::PushID(ci);
                    AnimCondition& cond = t.conditions[ci];
                    // param combo
                    ImGui::SetNextItemWidth(90.0f);
                    const char* pl = cond.paramName.empty() ? "(param)" : cond.paramName.c_str();
                    if (ImGui::BeginCombo("##cparam", pl)) {
                        for (const auto& pa : m_Working.params) {
                            const bool psel = (pa.name == cond.paramName);
                            if (ImGui::Selectable(pa.name.c_str(), psel) && pa.name != cond.paramName) { cond.paramName = pa.name; MarkEdited(); }
                            if (psel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    int op = (int)cond.op;
                    ImGui::SetNextItemWidth(90.0f);
                    if (ImGui::Combo("##cop", &op, opNames, 5)) { cond.op = (AnimCondOp)op; MarkEdited(); }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(70.0f);
                    if (ImGui::DragFloat("##cval", &cond.value, 0.05f)) MarkEdited();
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) removeCond = ci;
                    ImGui::PopID();
                }
                if (removeCond >= 0) { t.conditions.erase(t.conditions.begin() + removeCond); MarkEdited(); }
                if (ImGui::SmallButton("+ Add condition")) {
                    t.conditions.push_back(AnimCondition{ m_Working.params.empty() ? std::string() : m_Working.params.front().name, AnimCondOp::Greater, 0.0f });
                    MarkEdited();
                }
            }
        }
    } else {
        ImGui::TextDisabled("Select a transition to edit");
    }

    ImGui::EndChild(); // inspector

    ImGui::End();
}
