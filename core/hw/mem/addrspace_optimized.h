#pragma once
#include "types.h"
#include "addrspace.h"

// Forward declarations to access addrspace internals
extern void* memInfo_ptr[0x100];
#define HANDLER_MAX 0x1F

// Cache-line aligned lookup tables for better memory performance
namespace addrspace_opt {

/// Optimized memory access with reduced branching and better cache locality
template<typename T>
static inline T DYNACALL fast_readt(u32 addr) {
    // Optimize the hot path with prefetching and reduced lookup overhead
    u32 page = addr >> 24;
    uintptr_t lookup = (uintptr_t)memInfo_ptr[page];

    // Branch prediction hint: most accesses are to RAM
    if (likely((lookup & ~HANDLER_MAX) != 0)) {
        // Direct memory access - optimized path
        void *ptr = (void *)(lookup & ~HANDLER_MAX);
        addr <<= lookup;
        addr >>= lookup;

        // Prefetch next cache line for sequential access patterns
        __builtin_prefetch(&((u8 *)ptr)[addr + 64], 0, 3);

        return *(T *)&((u8 *)ptr)[addr];
    }

    // Fall back to original handler-based approach
    return addrspace::readt<T>(addr);
}

/// Optimized memory write with reduced overhead
template<typename T>
static inline void DYNACALL fast_writet(u32 addr, T data) {
    u32 page = addr >> 24;
    uintptr_t lookup = (uintptr_t)memInfo_ptr[page];

    if (likely((lookup & ~HANDLER_MAX) != 0)) {
        void *ptr = (void *)(lookup & ~HANDLER_MAX);
        addr <<= lookup;
        addr >>= lookup;

        // Prefetch for write
        __builtin_prefetch(&((u8 *)ptr)[addr], 1, 3);

        *(T *)&((u8 *)ptr)[addr] = data;
    } else {
        addrspace::writet<T>(addr, data);
    }
}

/// SIMD-optimized block memory operations
class FastMemOps {
public:
    /// Ultra-fast memcpy using NEON/SSE when available
    static void fast_memcpy(void* dst, const void* src, size_t size);

    /// Optimized memory zero using vector instructions
    static void fast_memzero(void* ptr, size_t size);

    /// Memory compare with early exit optimization
    static bool fast_memcmp(const void* ptr1, const void* ptr2, size_t size);

    /// Block copy with format conversion (e.g., endian swap)
    static void fast_block_copy_convert(void* dst, const void* src, size_t elements, int conversion_type);
};

/// Cache-aligned memory allocator for performance-critical structures
class CacheAlignedAllocator {
public:
    template<typename T>
    static T* allocate_aligned(size_t count = 1) {
        size_t size = sizeof(T) * count;
        size_t alignment = std::max(sizeof(T), size_t(64)); // Cache line alignment

#ifdef __APPLE__
        // Use posix_memalign on iOS for compatibility
        void* ptr = nullptr;
        if (posix_memalign(&ptr, alignment, (size + alignment - 1) & ~(alignment - 1)) == 0) {
            return (T*)ptr;
        }
        return nullptr;
#else
        return (T*)aligned_alloc(alignment, (size + alignment - 1) & ~(alignment - 1));
#endif
    }

    template<typename T>
    static void deallocate_aligned(T* ptr) {
        free(ptr);
    }
};

} // namespace addrspace_opt
