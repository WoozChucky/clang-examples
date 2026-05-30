#include "Framer.h"

#include <cstring>

#include "lib.h"  // SM_WARN

namespace netlib::detail {

bool Framer::Push(std::span<const std::byte> in, const OnFrame& onFrame) {
    m_Buf.insert(m_Buf.end(), in.begin(), in.end());

    size_t offset = 0;
    for (;;) {
        if (m_Buf.size() - offset < 4) break;  // not enough for a length prefix

        uint32_t len = 0;
        std::memcpy(&len, m_Buf.data() + offset, 4);  // x64 is little-endian; wire is LE

        if (len > m_Max) {
            SM_WARN("netlib: frame length %u exceeds max %u; disconnecting", len, m_Max);
            return false;
        }
        if (m_Buf.size() - offset - 4 < len) break;   // full payload not yet arrived

        const std::byte* payload = m_Buf.data() + offset + 4;
        onFrame(std::span<const std::byte>(payload, len));
        offset += 4 + len;
    }

    // Drop consumed bytes, keep the partial tail.
    if (offset > 0) m_Buf.erase(m_Buf.begin(), m_Buf.begin() + offset);
    return true;
}

} // namespace netlib::detail
