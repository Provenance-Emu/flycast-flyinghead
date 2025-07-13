#include "addrspace_optimized.h"
#include <cstring>
#include <cstdlib>

#ifdef __APPLE__
#include <unistd.h>  // For posix_memalign
#endif

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>
#define HAS_NEON 1
#elif defined(__SSE2__)
#include <emmintrin.h>
#include <immintrin.h>
#define HAS_SSE 1
#endif

namespace addrspace_opt {

/// Ultra-fast memcpy using SIMD instructions
void FastMemOps::fast_memcpy(void* dst, const void* src, size_t size) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;

#if HAS_NEON
    // NEON optimized path for ARM
    if (size >= 128 && ((uintptr_t)d % 16) == 0 && ((uintptr_t)s % 16) == 0) {
        // Process 128 bytes per iteration for maximum bandwidth
        size_t vectorized = (size / 128) * 128;

        for (size_t i = 0; i < vectorized; i += 128) {
            // Load 8x 16-byte vectors (128 bytes total)
            uint8x16_t v0 = vld1q_u8(s + i + 0);
            uint8x16_t v1 = vld1q_u8(s + i + 16);
            uint8x16_t v2 = vld1q_u8(s + i + 32);
            uint8x16_t v3 = vld1q_u8(s + i + 48);
            uint8x16_t v4 = vld1q_u8(s + i + 64);
            uint8x16_t v5 = vld1q_u8(s + i + 80);
            uint8x16_t v6 = vld1q_u8(s + i + 96);
            uint8x16_t v7 = vld1q_u8(s + i + 112);

            // Store all vectors
            vst1q_u8(d + i + 0, v0);
            vst1q_u8(d + i + 16, v1);
            vst1q_u8(d + i + 32, v2);
            vst1q_u8(d + i + 48, v3);
            vst1q_u8(d + i + 64, v4);
            vst1q_u8(d + i + 80, v5);
            vst1q_u8(d + i + 96, v6);
            vst1q_u8(d + i + 112, v7);
        }

        // Handle remaining bytes with regular memcpy
        size_t remaining = size - vectorized;
        if (remaining > 0) {
            memcpy(d + vectorized, s + vectorized, remaining);
        }
        return;
    }

    // Smaller NEON optimization for 16-64 byte copies
    if (size >= 16 && size <= 64) {
        for (size_t i = 0; i < size; i += 16) {
            size_t chunk = (size - i >= 16) ? 16 : (size - i);
            if (chunk == 16) {
                uint8x16_t v = vld1q_u8(s + i);
                vst1q_u8(d + i, v);
            } else {
                memcpy(d + i, s + i, chunk);
            }
        }
        return;
    }

#elif HAS_SSE
    // SSE optimized path for x86/x64
    if (size >= 64 && ((uintptr_t)d % 16) == 0 && ((uintptr_t)s % 16) == 0) {
        size_t vectorized = (size / 64) * 64;

        for (size_t i = 0; i < vectorized; i += 64) {
            __m128i v0 = _mm_load_si128((__m128i*)(s + i + 0));
            __m128i v1 = _mm_load_si128((__m128i*)(s + i + 16));
            __m128i v2 = _mm_load_si128((__m128i*)(s + i + 32));
            __m128i v3 = _mm_load_si128((__m128i*)(s + i + 48));

            _mm_store_si128((__m128i*)(d + i + 0), v0);
            _mm_store_si128((__m128i*)(d + i + 16), v1);
            _mm_store_si128((__m128i*)(d + i + 32), v2);
            _mm_store_si128((__m128i*)(d + i + 48), v3);
        }

        size_t remaining = size - vectorized;
        if (remaining > 0) {
            memcpy(d + vectorized, s + vectorized, remaining);
        }
        return;
    }
#endif

    // Fallback to standard memcpy
    memcpy(dst, src, size);
}

/// Optimized memory zero using vector instructions
void FastMemOps::fast_memzero(void* ptr, size_t size) {
    uint8_t* p = (uint8_t*)ptr;

#if HAS_NEON
    if (size >= 64 && ((uintptr_t)p % 16) == 0) {
        uint8x16_t zero = vdupq_n_u8(0);
        size_t vectorized = (size / 64) * 64;

        for (size_t i = 0; i < vectorized; i += 64) {
            vst1q_u8(p + i + 0, zero);
            vst1q_u8(p + i + 16, zero);
            vst1q_u8(p + i + 32, zero);
            vst1q_u8(p + i + 48, zero);
        }

        size_t remaining = size - vectorized;
        if (remaining > 0) {
            memset(p + vectorized, 0, remaining);
        }
        return;
    }

#elif HAS_SSE
    if (size >= 64 && ((uintptr_t)p % 16) == 0) {
        __m128i zero = _mm_setzero_si128();
        size_t vectorized = (size / 64) * 64;

        for (size_t i = 0; i < vectorized; i += 64) {
            _mm_store_si128((__m128i*)(p + i + 0), zero);
            _mm_store_si128((__m128i*)(p + i + 16), zero);
            _mm_store_si128((__m128i*)(p + i + 32), zero);
            _mm_store_si128((__m128i*)(p + i + 48), zero);
        }

        size_t remaining = size - vectorized;
        if (remaining > 0) {
            memset(p + vectorized, 0, remaining);
        }
        return;
    }
#endif

    // Fallback to standard memset
    memset(ptr, 0, size);
}

/// Memory compare with early exit optimization
bool FastMemOps::fast_memcmp(const void* ptr1, const void* ptr2, size_t size) {
    const uint8_t* p1 = (const uint8_t*)ptr1;
    const uint8_t* p2 = (const uint8_t*)ptr2;

#if HAS_NEON
    if (size >= 16) {
        size_t vectorized = (size / 16) * 16;

        for (size_t i = 0; i < vectorized; i += 16) {
            uint8x16_t v1 = vld1q_u8(p1 + i);
            uint8x16_t v2 = vld1q_u8(p2 + i);
            uint8x16_t cmp = vceqq_u8(v1, v2);

            // Check if all bytes are equal
            uint64x2_t result = vreinterpretq_u64_u8(cmp);
            if (vgetq_lane_u64(result, 0) != 0xFFFFFFFFFFFFFFFFULL ||
                vgetq_lane_u64(result, 1) != 0xFFFFFFFFFFFFFFFFULL) {
                return false; // Early exit on first difference
            }
        }

        // Compare remaining bytes
        for (size_t i = vectorized; i < size; i++) {
            if (p1[i] != p2[i]) return false;
        }
        return true;
    }

#elif HAS_SSE
    if (size >= 16) {
        size_t vectorized = (size / 16) * 16;

        for (size_t i = 0; i < vectorized; i += 16) {
            __m128i v1 = _mm_loadu_si128((__m128i*)(p1 + i));
            __m128i v2 = _mm_loadu_si128((__m128i*)(p2 + i));
            __m128i cmp = _mm_cmpeq_epi8(v1, v2);

            if (_mm_movemask_epi8(cmp) != 0xFFFF) {
                return false; // Early exit on first difference
            }
        }

        // Compare remaining bytes
        for (size_t i = vectorized; i < size; i++) {
            if (p1[i] != p2[i]) return false;
        }
        return true;
    }
#endif

    // Fallback to standard memcmp
    return memcmp(ptr1, ptr2, size) == 0;
}

/// Block copy with format conversion (optimized for common endian swaps)
void FastMemOps::fast_block_copy_convert(void* dst, const void* src, size_t elements, int conversion_type) {
    switch (conversion_type) {
    case 0: // No conversion - direct copy
        fast_memcpy(dst, src, elements);
        break;

    case 1: // 16-bit endian swap
        {
            uint16_t* d = (uint16_t*)dst;
            const uint16_t* s = (const uint16_t*)src;

#if HAS_NEON
            if (elements >= 8) {
                size_t vectorized = (elements / 8) * 8;

                for (size_t i = 0; i < vectorized; i += 8) {
                    uint16x8_t v = vld1q_u16(s + i);
                    uint16x8_t swapped = vrev16q_u8(vreinterpretq_u8_u16(v));
                    vst1q_u16(d + i, vreinterpretq_u16_u8(swapped));
                }

                // Handle remaining elements
                for (size_t i = vectorized; i < elements; i++) {
                    d[i] = __builtin_bswap16(s[i]);
                }
                return;
            }
#endif

            // Scalar fallback
            for (size_t i = 0; i < elements; i++) {
                d[i] = __builtin_bswap16(s[i]);
            }
        }
        break;

    case 2: // 32-bit endian swap
        {
            uint32_t* d = (uint32_t*)dst;
            const uint32_t* s = (const uint32_t*)src;

#if HAS_NEON
            if (elements >= 4) {
                size_t vectorized = (elements / 4) * 4;

                for (size_t i = 0; i < vectorized; i += 4) {
                    uint32x4_t v = vld1q_u32(s + i);
                    uint8x16_t bytes = vreinterpretq_u8_u32(v);
                    uint8x16_t swapped = vrev32q_u8(bytes);
                    vst1q_u32(d + i, vreinterpretq_u32_u8(swapped));
                }

                // Handle remaining elements
                for (size_t i = vectorized; i < elements; i++) {
                    d[i] = __builtin_bswap32(s[i]);
                }
                return;
            }
#endif

            // Scalar fallback
            for (size_t i = 0; i < elements; i++) {
                d[i] = __builtin_bswap32(s[i]);
            }
        }
        break;

    default:
        // Unknown conversion - just copy
        fast_memcpy(dst, src, elements);
        break;
    }
}

} // namespace addrspace_opt
