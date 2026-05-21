#pragma once

// Draws the "Memory" debug window from the Engine allocator registry.
// `open` may be null (always draw) or point to a toggle bool.
void DrawMemoryPanel(bool* open);
