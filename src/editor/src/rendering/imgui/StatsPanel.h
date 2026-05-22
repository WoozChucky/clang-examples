#pragma once
struct EditorContext;

// Draws the "Hello, world!" window: renderer/GPU/TPS readouts, GameThreadSettings editor,
// and frame-time stats.
class StatsPanel {
public:
    void Draw(const EditorContext& ctx);
};
