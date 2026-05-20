#pragma once
#include <cstddef>
#include <new>
#include <utility>

namespace Engine {

constexpr size_t AlignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Construct a T from any allocator exposing Allocate(size, align)/Deallocate.
template<class A, class T, class... Args>
T* New(A& alloc, Args&&... args) {
    void* p = alloc.Allocate(sizeof(T), alignof(T));
    return p ? ::new (p) T(std::forward<Args>(args)...) : nullptr;
}

template<class A, class T>
void Delete(A& alloc, T* ptr) {
    if (!ptr) return;
    ptr->~T();
    alloc.Deallocate(ptr, sizeof(T));
}

} // namespace Engine
