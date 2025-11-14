#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <lib.h>

/**
 * FrameAllocator - Linear allocator for per-frame transient memory allocations
 *
 * This allocator provides fast bump-pointer allocation from a pre-allocated buffer.
 * Memory is NOT freed individually - instead, the entire allocator is reset at the
 * end of each frame via Reset().
 *
 * Design:
 * - Pre-allocates a large buffer (default 16MB)
 * - Fast O(1) allocation with bump pointer
 * - No per-allocation overhead (no headers, no tracking)
 * - No destructor calls (transient allocations only)
 * - Reset once per frame to reclaim all memory
 *
 * Usage Example:
 *   FrameAllocator allocator;
 *
 *   // In render pass:
 *   auto* data = allocator.AllocateArray<UIInstanceCPU>(1024);
 *   // ... use data ...
 *
 *   // At end of frame:
 *   allocator.Reset();
 *
 * Thread Safety: Not thread-safe. Use per-thread allocators for parallel rendering.
 */
class FrameAllocator {
public:
    /**
     * Constructor
     * @param capacity Size of the pre-allocated buffer in bytes (default: 16MB)
     */
    explicit FrameAllocator(size_t capacity = 16 * 1024 * 1024)
        : m_Capacity(capacity)
    {
        m_Buffer = static_cast<uint8_t*>(std::malloc(capacity));
        if (!m_Buffer) {
            SM_ERROR("FrameAllocator: Failed to allocate %zu bytes", capacity);
            m_Capacity = 0;
            m_OwnsMemory = false;
        }
    }

    /**
     * Destructor - frees the pre-allocated buffer
     */
    ~FrameAllocator() {
        if (m_OwnsMemory && m_Buffer) {
            std::free(m_Buffer);
        }
        m_Buffer = nullptr;
        m_Capacity = 0;
        m_Offset = 0;
    }

    // Non-copyable
    FrameAllocator(const FrameAllocator&) = delete;
    FrameAllocator& operator=(const FrameAllocator&) = delete;

    /**
     * Allocate memory for a single object of type T
     * @return Pointer to allocated memory, or nullptr if out of memory
     */
    template<typename T>
    T* Allocate() {
        return static_cast<T*>(AllocateBytes(sizeof(T), alignof(T)));
    }

    /**
     * Allocate memory for an array of objects of type T
     * @param count Number of elements in the array
     * @return Pointer to allocated memory, or nullptr if out of memory
     */
    template<typename T>
    T* AllocateArray(size_t count) {
        if (count == 0) return nullptr;
        return static_cast<T*>(AllocateBytes(sizeof(T) * count, alignof(T)));
    }

    /**
     * Allocate raw bytes with specified alignment
     * @param size Number of bytes to allocate
     * @param alignment Alignment requirement (must be power of 2)
     * @return Pointer to aligned memory, or nullptr if out of memory
     */
    void* AllocateBytes(size_t size, size_t alignment = 16) {
        if (!m_Buffer || size == 0) {
            return nullptr;
        }

        // Align the current offset
        const size_t alignedOffset = AlignUp(m_Offset, alignment);
        const size_t newOffset = alignedOffset + size;

        // Check if allocation fits
        if (newOffset > m_Capacity) {
            SM_ERROR("FrameAllocator exhausted! Requested: %zu bytes (aligned: %zu), Used: %zu, Capacity: %zu, Usage: %.1f%%",
                     size, alignedOffset, m_Offset, m_Capacity, GetUsagePercent());
            return nullptr;
        }

        // Update offset and peak usage
        m_Offset = newOffset;
        if (m_Offset > m_PeakUsage) {
            m_PeakUsage = m_Offset;
        }

        // Return pointer to allocated memory
        return m_Buffer + alignedOffset;
    }

    /**
     * Reset the allocator for the next frame
     * This reclaims all allocated memory without freeing it.
     * Call this at the end of each frame.
     */
    void Reset() {
        m_Offset = 0;
        // Note: We keep m_PeakUsage for diagnostics across frames
    }

    /**
     * Get the number of bytes currently allocated this frame
     */
    [[nodiscard]] size_t GetUsedBytes() const {
        return m_Offset;
    }

    /**
     * Get the total capacity of the allocator in bytes
     */
    [[nodiscard]] size_t GetCapacity() const {
        return m_Capacity;
    }

    /**
     * Get the current usage as a percentage (0-100)
     */
    [[nodiscard]] float GetUsagePercent() const {
        if (m_Capacity == 0) return 0.0f;
        return (static_cast<float>(m_Offset) / static_cast<float>(m_Capacity)) * 100.0f;
    }

    /**
     * Get the peak usage across all frames since last reset of peak
     */
    [[nodiscard]] size_t GetPeakUsage() const {
        return m_PeakUsage;
    }

    /**
     * Reset peak usage tracking (useful for profiling specific sections)
     */
    void ResetPeakUsage() {
        m_PeakUsage = m_Offset;
    }

private:
    /**
     * Align a value up to the specified alignment
     * @param value Value to align
     * @param alignment Alignment (must be power of 2)
     * @return Aligned value
     */
    static size_t AlignUp(size_t value, size_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    uint8_t* m_Buffer;      // Pre-allocated memory buffer
    size_t m_Capacity;      // Total capacity in bytes
    size_t m_Offset{0};        // Current allocation offset (bump pointer)
    size_t m_PeakUsage{0};     // Peak usage across frames (for diagnostics)
    bool m_OwnsMemory{true};      // Whether this allocator owns the buffer
};

