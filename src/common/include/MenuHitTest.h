#pragma once
#include <cstdint>
#include <glm/vec2.hpp>

// Convert window-space mouse coords to UI/viewport space by subtracting the viewport origin
// (top-left of the scene viewport in window coords). In runtime the origin is (0,0) so this
// is identity; in the editor it offsets by the docked viewport image. (Phase 4 adds the
// rect hit-test alongside this.)
inline glm::vec2 ToUiSpace(double mouseX, double mouseY, uint32_t originX, uint32_t originY) {
    return glm::vec2(static_cast<float>(mouseX) - static_cast<float>(originX),
                     static_cast<float>(mouseY) - static_cast<float>(originY));
}

// True if point p is inside the half-open screen rect [pos, pos+size). Top-left inclusive,
// bottom-right exclusive — so adjacent rects don't both claim a shared edge pixel.
inline bool PointInRect(glm::vec2 p, glm::vec2 pos, glm::vec2 size) {
    return p.x >= pos.x && p.x < pos.x + size.x &&
           p.y >= pos.y && p.y < pos.y + size.y;
}
