#pragma once

struct EditorContext;

// Game-thread pacing controls (relocated from the old Hello panel): Target TPS presets + slider,
// spin-threshold slider, and the frame-time-tracking toggle. Reads/writes App->GameThreadConfig.
class SimulationPanel {
public:
    void Draw(const EditorContext& ctx);
};
