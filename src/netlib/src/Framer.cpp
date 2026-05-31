#include "Framer.h"

#include <cstring>

#include "lib.h"  // SM_WARN

namespace netlib::detail {

// Scan [0, m_Size) for complete [u32 len][payload] frames, emit each, compact the
// consumed prefix. Returns false on an oversize length prefix.
bool Framer::ScanAndCompact(const OnFrame& onFrame) {
    size_t offset = 0;
    for (;;) {
        if (m_Size - offset < 4) break;
        uint32_t len = 0;
        std::memcpy(&len, m_Buf.data() + offset, 4);   // x64 LE
        if (len > m_Max) {
            SM_WARN("netlib: frame length %u exceeds max %u; disconnecting", len, m_Max);
            return false;
        }
        if (m_Size - offset - 4 < len) break;
        const std::byte* payload = m_Buf.data() + offset + 4;
        onFrame(std::span<const std::byte>(payload, len));
        offset += 4 + len;
    }
    if (offset > 0) {
        const size_t remaining = m_Size - offset;
        if (remaining > 0) std::memmove(m_Buf.data(), m_Buf.data() + offset, remaining);
        m_Size = remaining;
    }
    return true;
}

bool Framer::Push(std::span<const std::byte> in, const OnFrame& onFrame) {
    std::byte* dst = PrepareRecv(in.size());
    if (!in.empty()) std::memcpy(dst, in.data(), in.size());
    return CommitRecv(in.size(), onFrame);
}

std::byte* Framer::PrepareRecv(size_t want) {
    if (m_Buf.size() < m_Size + want) m_Buf.resize(m_Size + want);
    return m_Buf.data() + m_Size;
}

bool Framer::CommitRecv(size_t got, const OnFrame& onFrame) {
    m_Size += got;
    return ScanAndCompact(onFrame);
}

} // namespace netlib::detail
