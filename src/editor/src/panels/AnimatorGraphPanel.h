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

    ax::NodeEditor::EditorContext* m_Ed = nullptr;
    uint64_t            m_ControllerId = 0;
    std::string         m_ControllerKey;
    std::string         m_SourcePath;
    AnimatorController  m_Working;
    AnimGraphLayout     m_Layout;
    bool                m_Dirty = false;
    bool                m_LayoutApplied = false;
    std::vector<std::string> m_Warnings;
};
