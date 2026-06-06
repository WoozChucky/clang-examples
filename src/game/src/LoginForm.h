#pragma once
#include <string>
#include <cstdint>

enum class LoginField : uint8_t { None, Username, Password };

// Game-owned login form state (runtime-only; not persisted, not inspector-registered).
// Cleared on Game.dll reload by the reload barrier and re-seeded if absent (Task 3).
struct LoginForm {
    std::string Username;
    std::string Password;
    std::string Error;
    LoginField  Focused = LoginField::None;
};
