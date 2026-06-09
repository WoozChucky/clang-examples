#pragma once

#include <nlohmann/json.hpp>

// Resolve the persisted AO mode (0=Off, 1=SSAO, 2=HBAO, 3=GTAO) from the settings "ssao" JSON
// object, migrating the legacy boolean "enabled" key.
//
// Precedence:
//   1. "mode" (int, in range [0,3])  -> use it
//   2. legacy "enabled" (bool)        -> true => SSAO(1), false => Off(0)
//   3. neither / out-of-range mode    -> default SSAO(1)
//
// Int-based + dependency-free (no engine enum) so it is unit-testable from the common settings
// layer; the Engine maps this int to/from its AoMode enum at the edges.
inline int ResolveAoMode(const nlohmann::json& ssao)
{
    if (ssao.contains("mode") && ssao["mode"].is_number_integer()) {
        const int m = ssao["mode"].get<int>();
        if (m >= 0 && m <= 3) return m;
    }
    if (ssao.contains("enabled") && ssao["enabled"].is_boolean()) {
        return ssao["enabled"].get<bool>() ? 1 : 0;
    }
    return 1; // default SSAO
}
