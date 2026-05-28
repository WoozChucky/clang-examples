#pragma once

struct EditorContext;

// Navigation panel: edit NavMeshConfigComponent singleton + trigger rebuild + show stats.
void DrawNavigationPanel(const EditorContext& ctx, bool* open);
