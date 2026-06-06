#pragma once
#include <cstddef>
#include <cstring>
#include <vector>

#include "input.h"
#include "ECS.h"       // InputStateComponent
#include "SpscRing.h"

// Drain ALL pending input events for this tick from `ring` into:
//  (1) `s` — the convenience InputStateComponent: per-tick fields (Pressed/MousePressed/
//            Wheel) are cleared, Key/Mouse events are digested, and mouse delta is computed
//            against `s`'s previous MouseX/MouseY. TextInput is intentionally NOT digested
//            here (the singleton has no field for it) — it is preserved raw in `outFrame`.
//  (2) `outFrame` — the raw, in-arrival-order event list INCLUDING TextInput; cleared first.
// Templated on ring capacity so GameThread's 256-deep ring and small test rings share one
// implementation. Owner-thread (GameThread) only.
template <std::size_t N>
inline void DrainInput(SpscRing<InputEvent, N>& ring, InputStateComponent& s,
                       std::vector<InputEvent>& outFrame) {
    const double prevX = s.MouseX;
    const double prevY = s.MouseY;

    std::memset(s.Pressed, 0, sizeof(s.Pressed));
    s.Wheel = 0;
    std::memset(s.MousePressed, 0, sizeof(s.MousePressed));

    outFrame.clear();
    InputEvent ev{};
    while (ring.Pop(ev)) {
        outFrame.push_back(ev); // raw, in order, including TextInput
        if (ev.Type == InputEventType::Key) {
            const int k = static_cast<int>(ev.KeyEvent.Key);
            if (k >= 0 && k <= KEY_LAST) {
                if (ev.KeyEvent.Action == PRESS || ev.KeyEvent.Action == REPEAT) s.KeysDown[k] = true;
                if (ev.KeyEvent.Action == RELEASE) s.KeysDown[k] = false;
                if (ev.KeyEvent.Action == PRESS) s.Pressed[k] = true;
            }
        } else if (ev.Type == InputEventType::MouseMove) {
            s.MouseX = ev.MouseMoveEvent.X;
            s.MouseY = ev.MouseMoveEvent.Y;
        } else if (ev.Type == InputEventType::MouseWheel) {
            s.Wheel = static_cast<int32_t>(ev.MouseScrollEvent.OffsetY);
        } else if (ev.Type == InputEventType::MouseButton) {
            const int b = static_cast<int>(ev.MouseButtonEvent.Button);
            if (b >= 0 && b <= MOUSE_BUTTON_LAST) {
                if (ev.MouseButtonEvent.Action == PRESS)   { s.MouseDown[b] = true; s.MousePressed[b] = true; }
                if (ev.MouseButtonEvent.Action == RELEASE) {  s.MouseDown[b] = false; }
            }
        }
    }
    s.MouseDX = s.MouseX - prevX;
    s.MouseDY = s.MouseY - prevY;
}
