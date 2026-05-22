#pragma once

// Draws the "Render Stats" debug window: per-frame mesh draw/cull counters and the
// frustum-culling on/off toggle. `open` may be null (always draw) or point to a toggle bool.
void DrawRenderStatsPanel(bool* open);
