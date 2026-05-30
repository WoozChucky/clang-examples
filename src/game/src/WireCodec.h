#pragma once

// Thin helpers bridging protobuf-lite messages to the netlib byte API
// (NetServices::Send(handle, conn, opcode, data, len) / NetEvent{payload,len}).
// Header-only, game-local. Wire framing is unchanged — these only (de)serialize
// the `data` field of an existing [u16 opcode][data] frame.

#include <cstdint>
#include <vector>

#include "wire.pb.h"   // generated into the build tree (gen/), on the game include path

namespace wirecodec {

// Serialize a protobuf-lite message to a byte buffer suitable for Send(data, len).
template <class M>
std::vector<uint8_t> Encode(const M& msg) {
    std::vector<uint8_t> out(msg.ByteSizeLong());
    if (!out.empty())
        msg.SerializeToArray(out.data(), static_cast<int>(out.size()));
    return out;
}

// Parse a NetEvent payload into a message. Returns false on malformed/truncated input.
template <class M>
bool Decode(const uint8_t* data, uint32_t len, M& out) {
    return out.ParseFromArray(data, static_cast<int>(len));
}

// Opcode trait: maps a message type to the wire::Opcode tag a send-site passes as
// the frame opcode. Specialize one line per message type.
template <class M> wire::Opcode OpcodeOf();
template <> inline wire::Opcode OpcodeOf<wire::Ping>()     { return wire::OPCODE_PING; }
template <> inline wire::Opcode OpcodeOf<wire::Pong>()     { return wire::OPCODE_PONG; }
template <> inline wire::Opcode OpcodeOf<wire::Snapshot>() { return wire::OPCODE_SNAPSHOT; }

} // namespace wirecodec
