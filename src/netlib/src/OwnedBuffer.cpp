#include "netlib/OwnedBuffer.h"

namespace netlib {

OwnedBuffer MakeHeapBuffer(uint32_t payloadLen) {
    auto* base = new std::byte[OwnedBuffer::kHeadBytes + payloadLen];
    return OwnedBuffer(base, payloadLen, /*ctx*/ nullptr,
                       [](void*, std::byte* b) noexcept { delete[] b; });
}

} // namespace netlib
