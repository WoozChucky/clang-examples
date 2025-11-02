#pragma once

#ifndef ALLOC_TRACKER_ENABLED
extern "C" inline void DumpAllocations() {}
#else

#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <atomic>

#if defined(_WIN32) || defined(_WIN64)
  #include <windows.h>
  #include <dbghelp.h>
  #pragma comment(lib, "dbghelp.lib")
#else
  #include <pthread.h>
  #include <execinfo.h>   // backtrace()
#endif

// Config
constexpr int kMaxBacktraceFrames = 16;
constexpr uint32_t kHeaderMagic = 0xA110C7ED; // "ALLOCATED" like magic

// Thread-local recursion guard
inline thread_local bool gInAllocator = false;

extern "C" {

// Minimal OS mutex wrapper (no allocations)
struct OsMutex {
#if defined(_WIN32) || defined(_WIN64)
    CRITICAL_SECTION cs;
    OsMutex() { InitializeCriticalSection(&cs); }
    ~OsMutex() { /* intentionally not deleted to avoid shutdown races */ }
    void Lock()   { EnterCriticalSection(&cs); }
    void Unlock() { LeaveCriticalSection(&cs); }
#else
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    void Lock()   { pthread_mutex_lock(&m); }
    void Unlock() { pthread_mutex_unlock(&m); }
#endif
};

// Allocation header stored just before the user pointer.
struct AllocationHeader {
    uint32_t Magic;
    void* RawPtr;                 // original pointer returned by malloc()
    std::size_t Size;
    const char* File;             // may be nullptr if not provided
    int Line;
    const char* Func;             // may be nullptr
    void* Backtrace[kMaxBacktraceFrames];
    int BacktraceSize;
    AllocationHeader* Next;
};

// Global state: head pointer and mutex. Intentionally leaked to avoid destructor order problems.
inline AllocationHeader*& GetHead() {
    static AllocationHeader* head = nullptr; // stored in data segment
    return head;
}
inline OsMutex& GetMutex() {
    static OsMutex* p = [](){
        // intentionally leak mutex to avoid destructors
        return new OsMutex();
    }();
    return *p;
}

// Backtrace capture (POSIX uses backtrace; Windows uses CaptureStackBackTrace if available)
inline int CaptureBacktrace(void** outBuf, int maxFrames) {
#if defined(_WIN32) || defined(_WIN64)
    // Try to use CaptureStackBackTrace (kernel32)
    // Note: CaptureStackBackTrace is available on modern Windows; if not, return 0.
    typedef USHORT (WINAPI *CapFn)(ULONG, ULONG, PVOID *, PULONG);
    HMODULE h = GetModuleHandleA("kernel32.dll");
    if (!h) return 0;
    CapFn cap = reinterpret_cast<CapFn>(GetProcAddress(h, "RtlCaptureStackBackTrace"));
    if (!cap) return 0;
    USHORT n = cap(0, (ULONG)maxFrames, outBuf, nullptr);
    return (int)n;
#else
    // POSIX
    int n = backtrace(outBuf, maxFrames);
    return n;
#endif
}

#if defined(_WIN32) || defined(_WIN64)

    inline void EnsureDbgHelpInitialized() {
        static std::atomic<bool> initialized = false;
        if (!initialized.load(std::memory_order_acquire)) {
            HANDLE process = GetCurrentProcess();
            SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
            if (SymInitialize(process, nullptr, TRUE)) {
                initialized.store(true, std::memory_order_release);
            }
        }
    }

    inline void PrintBacktrace(void** frames, int count) {
        EnsureDbgHelpInitialized();

        HANDLE process = GetCurrentProcess();
        SYMBOL_INFO* symInfo = (SYMBOL_INFO*)std::malloc(sizeof(SYMBOL_INFO) + 256);
        if (!symInfo) return;
        symInfo->MaxNameLen = 255;
        symInfo->SizeOfStruct = sizeof(SYMBOL_INFO);

        IMAGEHLP_LINE64 lineInfo;
        std::memset(&lineInfo, 0, sizeof(lineInfo));
        lineInfo.SizeOfStruct = sizeof(lineInfo);
        DWORD displacement = 0;

        for (int i = 0; i < count; ++i) {
            DWORD64 addr = (DWORD64)(frames[i]);
            if (SymFromAddr(process, addr, nullptr, symInfo)) {
                if (SymGetLineFromAddr64(process, addr, &displacement, &lineInfo)) {
                    std::fprintf(stderr, "  #%d %s (%s:%lu)\n",
                                 i, symInfo->Name, lineInfo.FileName, lineInfo.LineNumber);
                } else {
                    std::fprintf(stderr, "  #%d %s [0x%p]\n", i, symInfo->Name, (void*)addr);
                }
            } else {
                std::fprintf(stderr, "  #%d ??? [0x%p]\n", i, (void*)addr);
            }
        }

        std::free(symInfo);
    }
#endif

// Internal: place header and link into list
inline void InsertHeader(AllocationHeader* hdr) {
    OsMutex& mu = GetMutex();
    mu.Lock();
    hdr->Next = GetHead();
    GetHead() = hdr;
    mu.Unlock();
}

// Internal: unlink header by header pointer. Returns true if removed.
inline bool RemoveHeaderByPtr(AllocationHeader* hdr) {
    OsMutex& mu = GetMutex();
    mu.Lock();
    AllocationHeader** cur = &GetHead();
    while (*cur) {
        if (*cur == hdr) {
            *cur = hdr->Next;
            mu.Unlock();
            return true;
        }
        cur = &((*cur)->Next);
    }
    mu.Unlock();
    return false;
}

// Dump allocations (resolves symbols where possible). Call at any time.
inline void DumpAllocations() {
    OsMutex& mu = GetMutex();
    mu.Lock();
    AllocationHeader* cur = GetHead();
    std::size_t total = 0;
    while (cur) {
        ++total;
        // Print basic info
        const char* file = cur->File ? cur->File : "<unknown>";
        const char* func = cur->Func ? cur->Func : "<unknown>";
        std::fprintf(stderr, "LEAK: ptr=%p size=%zu %s:%d %s\n",
                     (void*)((char*)cur + sizeof(AllocationHeader)),
                     cur->Size, file, cur->Line, func);
#if !defined(_WIN32) && !defined(_WIN64)
        // POSIX: resolve symbols lazily
        if (cur->BacktraceSize > 0) {
            char** syms = backtrace_symbols(cur->Backtrace, cur->BacktraceSize);
            if (syms) {
                for (int i = 0; i < cur->BacktraceSize; ++i) {
                    std::fprintf(stderr, "  #%d %s\n", i, syms[i]);
                }
                free(syms); // backtrace_symbols allocates via malloc
            } else {
                for (int i = 0; i < cur->BacktraceSize; ++i)
                    std::fprintf(stderr, "  #%d %p\n", i, cur->Backtrace[i]);
            }
        }
#else
        // Windows: just print addresses (symbol resolution could be added)
        //for (int i = 0; i < cur->BacktraceSize; ++i)
        //    std::fprintf(stderr, "  #%d %p\n", i, cur->Backtrace[i]);
        PrintBacktrace(cur->Backtrace, cur->BacktraceSize);
#endif
        cur = cur->Next;
    }
    std::fprintf(stderr, "Total tracked blocks: %zu\n", total);
    mu.Unlock();
}

// Helper to compute aligned user pointer and header placement. Returns user pointer.
inline void* AllocateBlockWithHeader(std::size_t userSize, std::size_t alignment, AllocationHeader** outHdr) {
    // minimal alignment >= alignof(void*) for header pointer arithmetic safety
    if (alignment < alignof(void*)) alignment = alignof(void*);
    // We allocate extra: header + alignment padding
    std::size_t prefix = sizeof(AllocationHeader);
    std::size_t total = userSize + prefix + (alignment - 1);
    void* raw = std::malloc(total ? total : 1);
    if (!raw) return nullptr;
    // Compute aligned user pointer after header
    uintptr_t rawAddr = reinterpret_cast<uintptr_t>(raw);
    uintptr_t start = rawAddr + prefix;
    uintptr_t aligned = (start + (alignment - 1)) & ~(alignment - 1);
    AllocationHeader* hdr = reinterpret_cast<AllocationHeader*>(aligned - prefix);
    // initialize header in-place (header memory is part of the raw block, safe to write)
    hdr->Magic = kHeaderMagic;
    hdr->RawPtr = raw;
    hdr->Size = userSize;
    hdr->File = nullptr;
    hdr->Line = 0;
    hdr->Func = nullptr;
    hdr->BacktraceSize = 0;
    hdr->Next = nullptr;
    *outHdr = hdr;
    // return user pointer
    return reinterpret_cast<void*>(aligned);
}

} // extern "C"

// --- PUBLIC API / operator overloads ---

// Placement-style new that accepts source location
void* operator new(std::size_t size, const char* file, int line, const char* func) {
    if (size == 0) size = 1;
    if (gInAllocator) {
        void* p = std::malloc(size);
        if (!p) throw std::bad_alloc();
        return p;
    }
    gInAllocator = true;

    AllocationHeader* hdr = nullptr;
    void* userPtr = AllocateBlockWithHeader(size, alignof(std::max_align_t), &hdr);
    if (!userPtr) {
        gInAllocator = false;
        throw std::bad_alloc();
    }
    // fill location info (file/func are static strings from macros normally)
    hdr->File = file;
    hdr->Line = line;
    hdr->Func = func;
    // capture backtrace addresses
    hdr->BacktraceSize = CaptureBacktrace(hdr->Backtrace, kMaxBacktraceFrames);
    // Insert into global list
    InsertHeader(hdr);

    gInAllocator = false;
    return userPtr;
}

// Called if constructor throws (matching the placement new signature)
void operator delete(void* ptr, const char* /*file*/, int /*line*/, const char* /*func*/) noexcept {
    // This is invoked only if constructor throws after the placement new with extra args.
    // We'll free the block similarly to operator delete(void*).
    if (!ptr) return;
    // Reuse normal delete below:
    operator delete(ptr);
}

// Normal global new
void* operator new(std::size_t size) {
    if (size == 0) size = 1;
    if (gInAllocator) {
        void* p = std::malloc(size);
        if (!p) throw std::bad_alloc();
        return p;
    }
    gInAllocator = true;

    AllocationHeader* hdr = nullptr;
    void* userPtr = AllocateBlockWithHeader(size, alignof(std::max_align_t), &hdr);
    if (!userPtr) {
        gInAllocator = false;
        throw std::bad_alloc();
    }
    // No source info available for plain new
    hdr->File = nullptr;
    hdr->Line = 0;
    hdr->Func = nullptr;
    hdr->BacktraceSize = CaptureBacktrace(hdr->Backtrace, kMaxBacktraceFrames);
    InsertHeader(hdr);

    gInAllocator = false;
    return userPtr;
}

// Aligned allocation (C++17 signature)
void* operator new(std::size_t size, std::align_val_t align) {
    std::size_t alignment = static_cast<std::size_t>(align);
    if (size == 0) size = 1;
    if (gInAllocator) {
        void* p = std::malloc(size); // best-effort when re-entering
        if (!p) throw std::bad_alloc();
        return p;
    }
    gInAllocator = true;

    AllocationHeader* hdr = nullptr;
    void* userPtr = AllocateBlockWithHeader(size, alignment, &hdr);
    if (!userPtr) {
        gInAllocator = false;
        throw std::bad_alloc();
    }
    hdr->File = nullptr; hdr->Line = 0; hdr->Func = nullptr;
    hdr->BacktraceSize = CaptureBacktrace(hdr->Backtrace, kMaxBacktraceFrames);
    InsertHeader(hdr);

    gInAllocator = false;
    return userPtr;
}

// delete (standard)
void operator delete(void* ptr) noexcept {
    if (!ptr) return;
    if (gInAllocator) { std::free(ptr); return; }

    gInAllocator = true;
    // header is stored just before ptr
    AllocationHeader* hdr = reinterpret_cast<AllocationHeader*>(
        reinterpret_cast<char*>(ptr) - sizeof(AllocationHeader));
    // Basic sanity check
    if (hdr->Magic != kHeaderMagic) {
        // Possibly pointer wasn't allocated by our allocator: fallback to free
        std::free(ptr);
        gInAllocator = false;
        return;
    }
    // unlink from list (safe)
    RemoveHeaderByPtr(hdr);
    // free raw block
    std::free(hdr->RawPtr);

    gInAllocator = false;
}

// aligned delete
void operator delete(void* ptr, std::align_val_t) noexcept {
    operator delete(ptr);
}

#endif // ALLOC_TRACKER_ENABLED
