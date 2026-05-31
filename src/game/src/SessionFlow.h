#pragma once
#include <cstdint>

// Pure, socket-free client session state machine for the dual-server connection flow.
// ClientSessionSystem feeds it inputs (replies/events) and performs the I/O named by the
// returned action. No protobuf / NetServices dependency, so it is trivially unit-testable.

enum class SessionState : uint8_t {
    Disconnected,
    ConnectingAuth,
    Authenticating,
    WorldSelecting,
    WorldHandoff,
    SessionAuthing,
    CharSelecting,
    EnteringGame,
    InGame,
};

enum class SessionInput : uint8_t {
    AuthConnected,
    LoginOk, LoginFail,
    WorldListReceived,
    WorldSelectOk, WorldSelectFail,
    WorldConnected,
    SessionAuthOk, SessionAuthFail,
    CharListReceived,
    CharSelectOk, CharSelectFail,
    EnterGameOk, EnterGameFail,
    Dropped,
};

enum class SessionAction : uint8_t {
    None,
    SendLogin,
    SendWorldListReq,
    SendWorldSelect,
    BeginHandoff,
    SendSessionAuth,
    SendCharListReq,
    SendCharSelect,
    SendEnterGame,
    EnterGame,
    Reset,
};

struct SessionStep { SessionState next; SessionAction action; };

inline SessionStep AdvanceSession(SessionState s, SessionInput in) {
    using S = SessionState; using I = SessionInput; using A = SessionAction;

    switch (in) {
        case I::Dropped:
        case I::LoginFail:
        case I::WorldSelectFail:
        case I::SessionAuthFail:
        case I::CharSelectFail:
        case I::EnterGameFail:
            return { S::Disconnected, A::Reset };
        default: break;
    }

    switch (s) {
        case S::ConnectingAuth:
            if (in == I::AuthConnected)     return { S::Authenticating, A::SendLogin };
            break;
        case S::Authenticating:
            if (in == I::LoginOk)           return { S::WorldSelecting, A::SendWorldListReq };
            break;
        case S::WorldSelecting:
            if (in == I::WorldListReceived) return { S::WorldSelecting, A::SendWorldSelect };
            if (in == I::WorldSelectOk)     return { S::WorldHandoff,   A::BeginHandoff };
            break;
        case S::WorldHandoff:
            if (in == I::WorldConnected)    return { S::SessionAuthing, A::SendSessionAuth };
            break;
        case S::SessionAuthing:
            if (in == I::SessionAuthOk)     return { S::CharSelecting,  A::SendCharListReq };
            break;
        case S::CharSelecting:
            if (in == I::CharListReceived)  return { S::CharSelecting,  A::SendCharSelect };
            if (in == I::CharSelectOk)      return { S::EnteringGame,   A::SendEnterGame };
            break;
        case S::EnteringGame:
            if (in == I::EnterGameOk)       return { S::InGame,         A::EnterGame };
            break;
        default: break;
    }
    return { s, A::None };
}
