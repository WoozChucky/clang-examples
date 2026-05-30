#pragma once
#include <cstdint>

// Which role this process plays in the networking topology. Set by the bootstrap
// (GameThread => Client, ServerApplication => Server), carried on GameState and
// threaded into SystemContext so game systems can branch without linking Engine.
enum class AppRole : uint8_t {
    Client = 0,   // editor.exe / runtime.exe — connects out
    Server = 1,   // server.exe — listens
};
