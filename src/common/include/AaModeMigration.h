#pragma once

#include <nlohmann/json.hpp>

// Resolve the persisted anti-aliasing mode (0 = Off, 1 = FXAA, 2 = SMAA) from a settings
// "renderer" JSON object, migrating the legacy boolean "fxaa" key.
//
// Precedence:
//   1. "aaMode" (int, in range [0,2])         -> use it
//   2. legacy "fxaa" (bool)                    -> true => FXAA(1), false => Off(0)
//   3. neither / out-of-range aaMode           -> default FXAA(1)
//
// Kept dependency-free (int-based, no engine enum) so it is unit-testable and usable from
// the common settings layer; the Engine maps this int to/from its AAMode enum at the edges.
inline int ResolveAaMode(const nlohmann::json& renderer)
{
    if (renderer.contains("aaMode") && renderer["aaMode"].is_number_integer()) {
        const int m = renderer["aaMode"].get<int>();
        if (m >= 0 && m <= 2) return m;
    }
    if (renderer.contains("fxaa") && renderer["fxaa"].is_boolean()) {
        return renderer["fxaa"].get<bool>() ? 1 : 0;
    }
    return 1; // default FXAA
}
