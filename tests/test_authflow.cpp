#include <cassert>
#include <cstdio>
#include "SessionFlow.h"

using S = SessionState; using I = SessionInput; using A = SessionAction;

static int g_Failures = 0;
#define CHECK(cond, msg) do { if(!(cond)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,msg); ++g_Failures; } } while(0)

static void expect(S from, I in, S nextWant, A actWant, const char* tag) {
    SessionStep st = AdvanceSession(from, in);
    CHECK(st.next == nextWant && st.action == actWant, tag);
}

int main() {
    // Happy path, step by step.
    expect(S::ConnectingAuth, I::AuthConnected,     S::Authenticating, A::SendLogin,        "connected->login");
    expect(S::Authenticating, I::LoginOk,           S::WorldSelecting, A::SendWorldListReq, "loginok->worldlistreq");
    expect(S::WorldSelecting, I::WorldListReceived, S::WorldSelecting, A::SendWorldSelect,  "worldlist->select");
    expect(S::WorldSelecting, I::WorldSelectOk,     S::WorldHandoff,   A::BeginHandoff,     "selectok->handoff");
    expect(S::WorldHandoff,   I::WorldConnected,    S::SessionAuthing, A::SendSessionAuth,  "worldconn->sessionauth");
    expect(S::SessionAuthing, I::SessionAuthOk,     S::CharSelecting,  A::SendCharListReq,  "sessok->charlistreq");
    expect(S::CharSelecting,  I::CharListReceived,  S::CharSelecting,  A::SendCharSelect,   "charlist->select");
    expect(S::CharSelecting,  I::CharSelectOk,      S::EnteringGame,   A::SendEnterGame,    "charselok->entergame");
    expect(S::EnteringGame,   I::EnterGameOk,       S::InGame,         A::EnterGame,        "entergameok->ingame");

    // Failures + drops anywhere reset to Disconnected.
    expect(S::Authenticating, I::LoginFail,         S::Disconnected,  A::Reset, "loginfail->reset");
    expect(S::SessionAuthing, I::SessionAuthFail,   S::Disconnected,  A::Reset, "sessfail->reset");
    expect(S::WorldHandoff,   I::Dropped,           S::Disconnected,  A::Reset, "drop->reset");
    expect(S::CharSelecting,  I::CharSelectFail,    S::Disconnected,  A::Reset, "charfail->reset");
    expect(S::WorldSelecting, I::WorldSelectFail,   S::Disconnected,  A::Reset, "worldselectfail->reset");
    expect(S::EnteringGame,   I::EnterGameFail,     S::Disconnected,  A::Reset, "entergamefail->reset");

    // Unexpected input for a state is ignored (no spurious transition).
    expect(S::Authenticating, I::CharListReceived,  S::Authenticating, A::None, "unexpected->none");
    // Terminal/initial states: no outgoing transition on a normal input.
    expect(S::InGame,         I::EnterGameOk,       S::InGame,         A::None, "ingame-terminal->none");
    expect(S::Disconnected,   I::AuthConnected,     S::Disconnected,   A::None, "disconnected-noop->none");

    if (g_Failures == 0) { std::printf("All authflow tests passed.\n"); return 0; }
    std::printf("%d authflow test(s) FAILED.\n", g_Failures);
    return 1;
}
