#pragma once

// Draws the "Render Stats" debug window: per-frame mesh draw/cull counters and the
// debug toggles. `open` may be null (always draw) or point to a toggle bool.
// Returns true if any toggle/slider changed this frame (so the caller can persist).
bool DrawRenderStatsPanel(bool* open);
