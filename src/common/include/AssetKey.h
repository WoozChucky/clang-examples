#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Stable asset identity helpers. Pure + header-only so every module (ecs.dll, Engine, editor,
// game, tests) derives the SAME handle from the SAME logical key with no shared state. The logical
// key is a virtual path under assets/ (forward-slashed), e.g. "models/tree.obj" — the same
// addressing a future VFS uses. See docs/superpowers/specs/2026-06-07-stable-asset-identity-design.md.

// Reserved handle for the Missing mesh / magenta-checkerboard default material (slot 0).
inline constexpr uint64_t kMissingAssetHandle = 0ull;

// 64-bit FNV-1a over the key bytes. The empty key maps to kMissingAssetHandle explicitly (callers
// special-case empty before hashing); a non-empty key effectively never hashes to 0.
inline uint64_t AssetKeyHash(std::string_view key) {
    if (key.empty()) return kMissingAssetHandle;
    uint64_t h = 1469598103934665603ull;            // FNV offset basis
    for (unsigned char c : key) { h ^= c; h *= 1099511628211ull; } // FNV prime
    return h;
}

// Turn a filesystem path into a logical asset key: backslashes -> forward slashes, strip a leading
// "assets/" or "./". Pure string work (no <filesystem> dependency).
inline std::string NormalizeAssetKey(std::string path) {
    for (char& c : path) if (c == '\\') c = '/';
    if (path.rfind("./", 0) == 0) path.erase(0, 2);
    if (path.rfind("assets/", 0) == 0) path.erase(0, 7);
    return path;
}
