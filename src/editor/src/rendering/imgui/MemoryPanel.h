#pragma once

class ECS;

// Draws the "Memory" debug window: Engine allocator registry, snapshot pool, and
// (if `world` is non-null) ECS storage byte accounting.
// `open` may be null (always draw) or point to a toggle bool.
void DrawMemoryPanel(bool* open, const ECS* world);
