#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "AnimatorController.h"
#include "AnimatorGraphLayout.h"

struct EditorContext;
namespace ax { namespace NodeEditor { struct EditorContext; } }

// Top-level "Animator Graph" window: visual node-graph editor for AnimatorController assets.
// Owns an imgui-node-editor context + a mutable working copy of the selected controller.
class AnimatorGraphPanel {
public:
    AnimatorGraphPanel();
    ~AnimatorGraphPanel();
    AnimatorGraphPanel(const AnimatorGraphPanel&) = delete;
    AnimatorGraphPanel& operator=(const AnimatorGraphPanel&) = delete;
    void Draw(const EditorContext& ctx, bool* open);
private:
    void LoadController(uint64_t handle);
    void RecomputeWarnings();
    // Mark the working copy edited: set dirty + re-run validation. Call after EVERY mutation.
    void MarkEdited() { m_Dirty = true; RecomputeWarnings(); }
    // Map a canvas UID (stable per-state identity) back to its current index in m_Working.states; -1 if gone.
    int StateIndexForUid(uint32_t uid) const;
    // List of bare clip names available for this controller's model (derived from m_ControllerKey).
    std::vector<std::string> AvailableClips() const;

    ax::NodeEditor::EditorContext* m_Ed = nullptr;
    uint64_t            m_ControllerId = 0;
    std::string         m_ControllerKey;
    std::string         m_SourcePath;
    AnimatorController  m_Working;
    AnimGraphLayout     m_Layout;
    bool                m_Dirty = false;
    bool                m_LayoutApplied = false;
    std::vector<std::string> m_Warnings;

    // Stable per-state canvas identity, reordered/erased in lock-step with m_Working.states.
    // NOT serialized — purely the node-editor's node/pin id key (index churn would otherwise
    // carry node positions/selection to the wrong state after insert/delete/reorder).
    std::vector<uint32_t> m_StateUids;
    uint32_t              m_NextUid = 1;
    // Show the Any-State source node even when no from=="*" transition exists yet (so the user can
    // drag the first anyState link). Set true on "Add Any-State" and whenever such a transition exists.
    bool                  m_ShowAnyState = false;
};
