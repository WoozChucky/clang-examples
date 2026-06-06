#pragma once
#include <string>
#include "input.h"

// Apply one raw input event to a text field: append a printable TextInput codepoint
// (ASCII range; >= 32 and < 127), ignore everything else. Backspace/Enter/Tab are NOT
// handled here — the caller reads those from InputStateComponent.Pressed. Pure + testable.
inline void ApplyTextEdit(std::string& field, const InputEvent& ev) {
    if (ev.Type != InputEventType::TextInput) return;
    const uint32_t cp = ev.TextEvent.Key;
    if (cp >= 32 && cp < 127) field.push_back(static_cast<char>(cp));
}
