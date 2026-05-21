#pragma once

#include <lib.h>                    // MB()
#include <memory/ArenaAllocator.h>

// FrameAllocator: per-frame transient linear allocator. Now a thin subclass of
// Engine::ArenaAllocator (fixed capacity, bump, reset each frame). Kept as a
// distinct type so the `class FrameAllocator;` forward-decl in IRenderPass.h and
// all FrameAllocator* signatures stay valid — render code is unchanged.
class FrameAllocator : public Engine::ArenaAllocator {
public:
    explicit FrameAllocator(const size_t capacity = MB(16))
        : Engine::ArenaAllocator(capacity, Engine::MemCategory::FrameTransient, "Frame") {}
};
