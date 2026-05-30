#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace netlib::detail {

// Reassembles a [uint32 LE length][payload] byte stream into discrete frames.
// Stateful across Push() calls (buffers partial frames). Single-threaded: one
// Framer per connection, touched only by that connection's I/O thread.
class Framer {
public:
    explicit Framer(uint32_t maxFrameBytes) : m_Max(maxFrameBytes) {}

    using OnFrame = std::function<void(std::span<const std::byte>)>;

    // Feed received bytes. Calls `onFrame` once per complete frame. Returns false
    // if a length prefix exceeds maxFrameBytes (caller should disconnect); true otherwise.
    bool Push(std::span<const std::byte> in, const OnFrame& onFrame);

private:
    std::vector<std::byte> m_Buf;   // accumulated, not-yet-complete bytes
    uint32_t               m_Max;
};

} // namespace netlib::detail
