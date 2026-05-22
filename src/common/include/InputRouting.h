#pragma once
#include "Input.h" // InputEventType

// Decides whether an input event should be routed to the game ring (in addition to ImGui, which
// always receives it). cursorLocked => play/FPS mode: the game owns everything. Otherwise mouse
// events are gated by acceptsMouse (Viewport hovered & no gizmo drag) and keyboard/text events by
// acceptsKeyboard (Viewport focused & no text field). The default branch is a defensive fallback
// for any future InputEventType (the current five values are all covered above).
inline bool RouteInputToGame(InputEventType type, bool cursorLocked,
                             bool acceptsMouse, bool acceptsKeyboard)
{
    if (cursorLocked)
        return true;
    switch (type) {
        case InputEventType::MouseMove:
        case InputEventType::MouseButton:
        case InputEventType::MouseWheel:
            return acceptsMouse;
        case InputEventType::Key:
        case InputEventType::TextInput:
            return acceptsKeyboard;
        default:
            return true;
    }
}
