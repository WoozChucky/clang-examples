#pragma once

#include <cstddef>
#include <cstdint>

#include "netlib/netlib_api.h"

namespace netlib {

// Move-only buffer with a reserved length-prefix head. The producer writes the
// payload into Payload()[0 .. PayloadLen); the transport writes the 4-byte LE
// length prefix into the head and sends [Data(), Data()+TotalSize()). On
// destruction (or move-assign overwrite) the deleter reclaims the backing memory
// — this is what lets the transport stay agnostic to where the bytes came from
// (a pool block, a heap allocation, a test buffer).
class NETLIB_API OwnedBuffer {
public:
    static constexpr uint32_t kHeadBytes = 4;
    // ctx is opaque to the buffer; del receives it plus the base pointer.
    using Deleter = void (*)(void* ctx, std::byte* base) noexcept;

    OwnedBuffer() noexcept = default;
    // base points at the head (kHeadBytes reserved); base..base+kHeadBytes+payloadLen must be valid.
    OwnedBuffer(std::byte* base, uint32_t payloadLen, void* ctx, Deleter del) noexcept
        : m_Base(base), m_PayloadLen(payloadLen), m_Ctx(ctx), m_Del(del) {}

    OwnedBuffer(OwnedBuffer&& o) noexcept { MoveFrom(o); }
    OwnedBuffer& operator=(OwnedBuffer&& o) noexcept {
        if (this != &o) { Reset(); MoveFrom(o); }
        return *this;
    }
    OwnedBuffer(const OwnedBuffer&) = delete;
    OwnedBuffer& operator=(const OwnedBuffer&) = delete;
    ~OwnedBuffer() { Reset(); }

    [[nodiscard]] bool       Valid()      const noexcept { return m_Base != nullptr; }
    [[nodiscard]] std::byte* Data()       const noexcept { return m_Base; }                  // head + payload
    [[nodiscard]] std::byte* Payload()    const noexcept { return m_Base + kHeadBytes; }     // writable payload
    [[nodiscard]] uint32_t   PayloadLen() const noexcept { return m_PayloadLen; }
    [[nodiscard]] uint32_t   TotalSize()  const noexcept { return kHeadBytes + m_PayloadLen; }

private:
    void Reset() noexcept {
        if (m_Base && m_Del) m_Del(m_Ctx, m_Base);
        m_Base = nullptr; m_Del = nullptr; m_Ctx = nullptr; m_PayloadLen = 0;
    }
    void MoveFrom(OwnedBuffer& o) noexcept {
        m_Base = o.m_Base; m_PayloadLen = o.m_PayloadLen; m_Ctx = o.m_Ctx; m_Del = o.m_Del;
        o.m_Base = nullptr; o.m_Del = nullptr; o.m_Ctx = nullptr; o.m_PayloadLen = 0;
    }

    std::byte* m_Base = nullptr;
    uint32_t   m_PayloadLen = 0;
    void*      m_Ctx = nullptr;
    Deleter    m_Del = nullptr;
};

// Heap-backed OwnedBuffer of (kHeadBytes + payloadLen) bytes; deleter = delete[].
// Used by tests and by the engine's >16 KB send fallback.
NETLIB_API OwnedBuffer MakeHeapBuffer(uint32_t payloadLen);

} // namespace netlib
