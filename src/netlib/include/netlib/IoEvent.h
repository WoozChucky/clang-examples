#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "netlib/Endpoint.h"

namespace netlib {

// One framed message OR a connection-lifecycle notification, pushed by the adapter
// into IIoSink::OnIoEvent from an adapter-owned thread.
struct IoEvent {
    enum class Kind : uint8_t { Connected, Disconnected, Error, Message };

    Kind   kind = Kind::Error;
    ConnId conn = ConnId::Invalid;
    // Valid only for kind == Message, and only for the duration of the OnIoEvent
    // call (borrowed — the adapter owns the buffer). Copy it before returning.
    std::span<const std::byte> payload;
};

} // namespace netlib
