#include "AnimatorGraphPanel.h"

#include <imgui.h>
#include <imgui_node_editor.h>

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <functional>
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

void AnimatorGraphPanel::SaveController() {
    if (m_ControllerId == 0 || m_SourcePath.empty()) {
        SM_WARN("AnimatorGraphPanel: no controller / no source path — cannot save");
        return;
    }
    // 1. Capture current node positions into m_Layout (keyed by state NAME), using UID-based node ids.
    ed::SetCurrentEditor(m_Ed);
    for (size_t i = 0; i < m_Working.states.size(); ++i) {
        const ImVec2 p = ed::GetNodePosition(NodeId(m_StateUids[i]));
        m_Layout.nodes[m_Working.states[i].name] = { p.x, p.y };
    }
    { const ImVec2 ap = ed::GetNodePosition(NodeId(kAnyStateNode)); m_Layout.nodes["__any__"] = { ap.x, ap.y }; }
    ed::SetCurrentEditor(nullptr);
    // 2. Serialize graph (to_json) + merge editorLayout.
    nlohmann::json doc = m_Working;          // to_json(AnimatorController) — emits name/params/states/transitions
    WriteLayout(doc, m_Layout);
    // 3. Write the source .animctrl.json.
    std::ofstream f(m_SourcePath);
    if (!f) { SM_WARN("AnimatorGraphPanel: failed to open '%s' for write", m_SourcePath.c_str()); return; }
    f << doc.dump(2);
    f.close();
    // 4. Resolve clip names (mirror the GameThread drain) + live-reload the store.
    AnimatorController resolved = m_Working;
    const std::string suffix = "#animctrl";
    const std::string assetKey = (m_ControllerKey.size() >= suffix.size())
        ? m_ControllerKey.substr(0, m_ControllerKey.size() - suffix.size()) : m_ControllerKey;
    resolved.stateClipIds.assign(resolved.states.size(), 0);
    for (size_t s = 0; s < resolved.states.size(); ++s) {
        if (resolved.states[s].clipKey.empty()) continue;
        resolved.stateClipIds[s] = AssetKeyHash(assetKey + "#anim/" + resolved.states[s].clipKey);
    }
    AnimatorControllerStore::Instance().Reload(m_ControllerKey, std::move(resolved));
    m_Dirty = false;
    SM_TRACE("AnimatorGraphPanel: saved + reloaded '%s' -> %s", m_ControllerKey.c_str(), m_SourcePath.c_str());
}

void AnimatorGraphPanel::ReloadFromDisk() {
    if (m_SourcePath.empty()) { SM_WARN("AnimatorGraphPanel: no source path to reload"); return; }
    std::ifstream f(m_SourcePath);
    if (!f) { SM_WARN("AnimatorGraphPanel: failed to open '%s'", m_SourcePath.c_str()); return; }
    nlohmann::json doc;
    try { f >> doc; } catch (const std::exception& ex) {
        SM_WARN("AnimatorGraphPanel: parse '%s': %s", m_SourcePath.c_str(), ex.what()); return;
    }
    m_Working = doc.get<AnimatorController>();
    m_Layout  = ReadLayout(doc);
    // Rebuild the stable uids for the freshly-loaded states (same as LoadController does).
    m_StateUids.assign(m_Working.states.size(), 0);
    for (auto& u : m_StateUids) u = m_NextUid++;
    m_ShowAnyState = false;
    for (const auto& t : m_Working.transitions) if (t.from == "*") { m_ShowAnyState = true; break; }
    m_LayoutApplied = false;
    m_Dirty = false;
    RecomputeWarnings();
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
    if (ImGui::Button("Save")) m_SavePending = true; // deferred: run after the canvas ed::End() so node positions are final
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reload from disk")) ReloadFromDisk();
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

    // state nodes
    // Node bodies are DISPLAY-ONLY: no interactive widgets. imgui-node-editor applies a
    // canvas pan/zoom transform, so any ImGui popup (BeginCombo) opened inside ed::BeginNode
    // is positioned in canvas space and renders far off-screen. All per-state editing lives in
    // the selected-node inspector (post-canvas, off the transformed canvas) instead.
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

        // name (+ entry marker)
        if (s == 0) ImGui::Text("%s (entry)", st.name.c_str());
        else        ImGui::Text("%s", st.name.c_str());

        // clip (read-only)
        ImGui::TextDisabled("%s", st.clipKey.empty() ? "(no clip)" : st.clipKey.c_str());

        // cyclic / loop (read-only markers)
        if (st.cyclic || st.loop)
            ImGui::TextDisabled("%s%s", st.cyclic ? "[cyclic] " : "", st.loop ? "[loop]" : "");

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
    // imgui-node-editor can report multiple deletions in a single frame (box-select + Del).
    // Erasing inline mid-loop shifts the vectors and invalidates the index-based link ids,
    // so we only COLLECT here and apply a single coherent removal pass after EndDelete().
    std::vector<uint32_t> deletedNodeUids;   // states the user asked to delete
    std::vector<int>      deletedLinkTis;    // transition indices from explicit link deletes
    if (ed::BeginDelete()) {
        ed::NodeId delNode;
        while (ed::QueryDeletedNode(&delNode)) {
            const uint32_t uid = static_cast<uint32_t>(delNode.Get());
            if (uid == kAnyStateNode) { ed::RejectDeletedItem(); continue; } // anyState isn't a real state
            const int idx = StateIndexForUid(uid);
            if (idx >= 0 && ed::AcceptDeletedItem()) {
                deletedNodeUids.push_back(uid);
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
                    deletedLinkTis.push_back(ti);
                } else {
                    ed::RejectDeletedItem();
                }
            } else {
                ed::RejectDeletedItem();
            }
        }
    }
    ed::EndDelete();

    // Apply collected deletions once, in a way that doesn't invalidate indices mid-pass.
    if (!deletedNodeUids.empty() || !deletedLinkTis.empty()) {
        // 1. Resolve node uids -> state indices, collect their names, then erase states/uids/layout
        //    in DESCENDING index order so earlier erases don't shift later indices.
        std::vector<std::string> deletedNames;
        std::vector<int>         nodeIdxs;
        for (uint32_t uid : deletedNodeUids) {
            const int idx = StateIndexForUid(uid);
            if (idx >= 0) { nodeIdxs.push_back(idx); deletedNames.push_back(m_Working.states[idx].name); }
        }
        std::sort(nodeIdxs.begin(), nodeIdxs.end(), std::greater<int>());
        nodeIdxs.erase(std::unique(nodeIdxs.begin(), nodeIdxs.end()), nodeIdxs.end());
        for (int idx : nodeIdxs) {
            if (idx >= 0 && idx < (int)m_Working.states.size()) {
                m_Layout.nodes.erase(m_Working.states[idx].name);
                m_Working.states.erase(m_Working.states.begin() + idx);
                m_StateUids.erase(m_StateUids.begin() + idx);
            }
        }

        // 2. Build the final transition-removal index set: explicit link deletes PLUS any
        //    transition referencing a deleted state name. Dedup, sort DESCENDING, erase once.
        std::vector<int> txToErase = deletedLinkTis;
        for (int ti = 0; ti < (int)m_Working.transitions.size(); ++ti) {
            const AnimTransition& t = m_Working.transitions[ti];
            for (const std::string& n : deletedNames) {
                if (t.from == n || t.to == n) { txToErase.push_back(ti); break; }
            }
        }
        std::sort(txToErase.begin(), txToErase.end(), std::greater<int>());
        txToErase.erase(std::unique(txToErase.begin(), txToErase.end()), txToErase.end());
        for (int ti : txToErase) {
            if (ti >= 0 && ti < (int)m_Working.transitions.size())
                m_Working.transitions.erase(m_Working.transitions.begin() + ti);
        }

        MarkEdited();
    }

    // capture selection (for the inspector) before End()
    ed::NodeId selNodes[1];
    const int selNodeCount = ed::GetSelectedNodes(selNodes, 1);
    ed::LinkId selLinks[1];
    const int selLinkCount = ed::GetSelectedLinks(selLinks, 1);

    ed::End();
    ed::SetCurrentEditor(nullptr);

    if (liveFrom >= 0)
        ImGui::Text("Active transition weight: %.2f", liveW);

    ImGui::EndChild(); // canvas

    // --- deferred save: runs after the canvas ed::End() (current editor already cleared above),
    //     so node positions / renames are final. ---
    if (m_SavePending) { m_SavePending = false; SaveController(); }

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

    // --- selection inspector: a NODE takes priority over a LINK ---
    // Decode a selected node -> state index (kAnyStateNode and stale ids resolve to no real state).
    int selStateIdx = -1;
    if (selNodeCount == 1) {
        const uint32_t uid = static_cast<uint32_t>(selNodes[0].Get());
        if (uid != kAnyStateNode) selStateIdx = StateIndexForUid(uid);
    }

    if (selStateIdx >= 0 && selStateIdx < (int)m_Working.states.size()) {
        // --- selected-node inspector (off-canvas: combo/inputtext popups position correctly) ---
        const int i = selStateIdx;
        AnimState& st = m_Working.states[i];
        ImGui::Text("Selected state%s", i == 0 ? " (entry)" : "");

        // Name: frame-local buffer seeded from the state name. ImGui keeps its own edit buffer
        // while the field is focused (reads this one only on (re)activation), so re-seeding each
        // frame is safe and doesn't clobber in-progress typing.
        char nameBuf[64];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", st.name.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
            std::string nn = nameBuf;
            if (!nn.empty() && nn != st.name) {
                const std::string oldName = st.name;
                // move layout entry old->new, then rewrite states + transitions
                auto it = m_Layout.nodes.find(oldName);
                std::array<float,2> xy = (it != m_Layout.nodes.end()) ? it->second : std::array<float,2>{0.0f, 0.0f};
                if (it != m_Layout.nodes.end()) m_Layout.nodes.erase(it);
                m_Layout.nodes[nn] = xy;
                RenameState(m_Working, oldName, nn);
                MarkEdited();
            }
        }

        // Clip dropdown (now outside the canvas transform).
        ImGui::SetNextItemWidth(-FLT_MIN);
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

        if (i != 0 && ImGui::Button("Set as entry")) {
            // rotate state i -> index 0; keep its uid aligned in lock-step.
            AnimState moved = m_Working.states[i];
            uint32_t  u     = m_StateUids[i];
            m_Working.states.erase(m_Working.states.begin() + i);
            m_StateUids.erase(m_StateUids.begin() + i);
            m_Working.states.insert(m_Working.states.begin(), moved);
            m_StateUids.insert(m_StateUids.begin(), u);
            MarkEdited();
        }
    }
    // Selected-transition inspector
    else if (selLinkCount == 1) {
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
        ImGui::TextDisabled("Select a node or transition to edit");
    }

    ImGui::EndChild(); // inspector

    ImGui::End();
}
