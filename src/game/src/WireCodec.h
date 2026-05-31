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
        (void)msg.SerializeToArray(out.data(), static_cast<int>(out.size()));
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

// Serialize [u16 opcode][protobuf] for `msg` straight into `dst` (cap bytes).
// opcode = OpcodeOf<M>(). Returns total bytes written (2 + ByteSize), or 0 if cap
// is too small. The caller sizes dst via AcquireSend(2 + msg.ByteSizeLong()).
template <class M>
uint32_t EncodeInto(uint8_t* dst, uint32_t cap, const M& msg) {
    const uint32_t n = static_cast<uint32_t>(msg.ByteSizeLong());
    if (cap < 2u + n) return 0;
    const uint16_t op = static_cast<uint16_t>(OpcodeOf<M>());
    dst[0] = static_cast<uint8_t>(op & 0xff);
    dst[1] = static_cast<uint8_t>((op >> 8) & 0xff);
    if (n) (void)msg.SerializeToArray(dst + 2, static_cast<int>(n));
    return 2u + n;
}

// Read the leading [u16 opcode] from a received frame (0 if len < 2).
inline uint16_t PeekOpcode(const uint8_t* frame, uint32_t len) {
    return len >= 2 ? static_cast<uint16_t>(frame[0]) |
                      (static_cast<uint16_t>(frame[1]) << 8)
                    : 0;
}

} // namespace wirecodec
