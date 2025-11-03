#include "Game.h"

uint32_t GameGetVersion() {
    SM_TRACE("GameGetVersion");
    return 0;
}

void GameSetPlatformDebugBreak(GameDebugBreakFn fn) {
    SM_TRACE("GameSetPlatformDebugBreak");
}

void GameUpdate(GameState* state) {
    SM_TRACE("GameUpdate");
}

void GameResize(uint32_t width, uint32_t height) {
    SM_TRACE("GameResize: %ux%u", width, height);
}

void GameExit() {
    SM_TRACE("GameExit");
}