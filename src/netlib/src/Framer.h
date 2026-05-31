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

    // Recv-in-place API: reserve `want` bytes at the buffer tail and return a write
    // pointer; the caller WSARecv's into it, then calls CommitRecv(got, onFrame).
    // Eliminates the separate recv-scratch + insert copy.
    std::byte* PrepareRecv(size_t want);
    // Advance the buffer by `got` received bytes, emit every complete frame, compact
    // the consumed prefix. Returns false on an oversize length prefix (disconnect).
    bool CommitRecv(size_t got, const OnFrame& onFrame);

private:
    // Scan [0, m_Size) for complete frames, emit each, compact the consumed prefix.
    bool ScanAndCompact(const OnFrame& onFrame);

    std::vector<std::byte> m_Buf;   // accumulated bytes (may have reserved tail capacity)
    size_t                 m_Size = 0;   // valid bytes in m_Buf (m_Buf may have extra reserved tail capacity)
    uint32_t               m_Max;
};

} // namespace netlib::detail
