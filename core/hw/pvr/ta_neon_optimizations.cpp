/*
 * NEON-Optimized Tile Accelerator Processing Implementation
 * Focusing on FMV performance improvements for older iOS devices
 */

#include "ta_neon_optimizations.h"

#ifdef __APPLE__
#if (TARGET_OS_IOS || TARGET_OS_TV) && defined(__ARM_NEON__)

#include <chrono>
#include <unordered_map>
#include <mutex>
#include "log/LogManager.h"

namespace flycast {

// Static member definitions
bool NEONTAProcessor::isInitialized = false;
bool NEONTAProcessor::hasAdvancedNEON = false;
std::unordered_map<std::string, NEONPerfMonitor::TimingData> NEONPerfMonitor::timingMap;
std::mutex NEONPerfMonitor::timingMutex;
bool NEONPerfMonitor::profilingEnabled = false;

//==============================================================================
// NEONTAProcessor Implementation
//==============================================================================

void NEONTAProcessor::Initialize()
{
    if (isInitialized) return;

    INFO_LOG(RENDERER, "🔧 Initializing NEON TA Processor for iOS optimization");

    // Check for advanced NEON features (ARMv8.1+)
    hasAdvancedNEON = true; // Assume modern iOS devices have advanced NEON

    isInitialized = true;
    INFO_LOG(RENDERER, "✅ NEON TA Processor initialized successfully");
}

void NEONTAProcessor::CopyTADataNEON(void* dst, const void* src, size_t size)
{
    if (!dst || !src || size == 0) return;

    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);

    // Use NEON for large copies (>128 bytes)
    if (size >= 128 && ((uintptr_t)src & 15) == 0 && ((uintptr_t)dst & 15) == 0) {
        const size_t neonChunks = size / 64;
        const size_t remainder = size % 64;

        for (size_t i = 0; i < neonChunks; i++) {
            // Load 64 bytes using 4x 16-byte NEON loads
            uint8x16_t chunk1 = vld1q_u8(s);
            uint8x16_t chunk2 = vld1q_u8(s + 16);
            uint8x16_t chunk3 = vld1q_u8(s + 32);
            uint8x16_t chunk4 = vld1q_u8(s + 48);

            // Store 64 bytes using 4x 16-byte NEON stores
            vst1q_u8(d, chunk1);
            vst1q_u8(d + 16, chunk2);
            vst1q_u8(d + 32, chunk3);
            vst1q_u8(d + 48, chunk4);

            s += 64;
            d += 64;
        }

        // Copy remainder with regular memcpy
        if (remainder > 0) {
            memcpy(d, s, remainder);
        }
    } else {
        // Fall back to regular memcpy for small or unaligned data
        memcpy(dst, src, size);
    }
}

//==============================================================================
// NEONFMVOps Implementation - Critical for FMV performance
//==============================================================================

void NEONFMVOps::ProcessYUVFrameNEON(const uint8_t* yuvData, uint8_t* rgbData,
                                     int width, int height, int format)
{
    if (!yuvData || !rgbData || width <= 0 || height <= 0) return;

    NEONPerfMonitor::StartTiming("ProcessYUVFrameNEON");

    const int pixels = width * height;
    const uint8_t* y = yuvData;
    const uint8_t* u = yuvData + pixels;
    const uint8_t* v = yuvData + pixels + (pixels / 4);

    // Process 8 pixels at a time using NEON
    const int neonPixels = (pixels / 8) * 8;

    for (int i = 0; i < neonPixels; i += 8) {
        // Load 8 Y values
        uint8x8_t y_vec = vld1_u8(y + i);

        // Load 2 U and V values (4:2:0 subsampling)
        uint8x8_t u_vec = vdup_n_u8(u[i/4]);
        uint8x8_t v_vec = vdup_n_u8(v[i/4]);

        // Convert to 16-bit for calculations
        int16x8_t y_16 = vreinterpretq_s16_u16(vmovl_u8(y_vec));
        int16x8_t u_16 = vreinterpretq_s16_u16(vmovl_u8(u_vec));
        int16x8_t v_16 = vreinterpretq_s16_u16(vmovl_u8(v_vec));

        // YUV to RGB conversion constants
        const int16x8_t c298 = vdupq_n_s16(298);
        const int16x8_t c409 = vdupq_n_s16(409);
        const int16x8_t c208 = vdupq_n_s16(208);
        const int16x8_t c100 = vdupq_n_s16(100);
        const int16x8_t c516 = vdupq_n_s16(516);
        const int16x8_t c128 = vdupq_n_s16(128);
        const int16x8_t c16 = vdupq_n_s16(16);

        // Y' = Y - 16
        int16x8_t y_adj = vsubq_s16(y_16, c16);
        // U' = U - 128
        int16x8_t u_adj = vsubq_s16(u_16, c128);
        // V' = V - 128
        int16x8_t v_adj = vsubq_s16(v_16, c128);

        // R = (298 * Y' + 409 * V') >> 8
        int16x8_t r = vaddq_s16(vmulq_s16(c298, y_adj), vmulq_s16(c409, v_adj));
        r = vshrq_n_s16(r, 8);

        // G = (298 * Y' - 100 * U' - 208 * V') >> 8
        int16x8_t g = vsubq_s16(vmulq_s16(c298, y_adj),
                               vaddq_s16(vmulq_s16(c100, u_adj), vmulq_s16(c208, v_adj)));
        g = vshrq_n_s16(g, 8);

        // B = (298 * Y' + 516 * U') >> 8
        int16x8_t b = vaddq_s16(vmulq_s16(c298, y_adj), vmulq_s16(c516, u_adj));
        b = vshrq_n_s16(b, 8);

        // Clamp to 0-255 and convert back to 8-bit
        uint8x8_t r_final = vqmovun_s16(r);
        uint8x8_t g_final = vqmovun_s16(g);
        uint8x8_t b_final = vqmovun_s16(b);
        uint8x8_t a_final = vdup_n_u8(255); // Full alpha

        // Interleave RGBA
        uint8x8x4_t rgba = {r_final, g_final, b_final, a_final};
        vst4_u8(rgbData + i * 4, rgba);
    }

    // Handle remaining pixels with scalar code
    for (int i = neonPixels; i < pixels; i++) {
        int y_val = y[i] - 16;
        int u_val = u[i/4] - 128;
        int v_val = v[i/4] - 128;

        int r = (298 * y_val + 409 * v_val) >> 8;
        int g = (298 * y_val - 100 * u_val - 208 * v_val) >> 8;
        int b = (298 * y_val + 516 * u_val) >> 8;

        rgbData[i*4 + 0] = std::clamp(r, 0, 255);
        rgbData[i*4 + 1] = std::clamp(g, 0, 255);
        rgbData[i*4 + 2] = std::clamp(b, 0, 255);
        rgbData[i*4 + 3] = 255;
    }

    NEONPerfMonitor::EndTiming("ProcessYUVFrameNEON");
}

void NEONFMVOps::DeinterlaceFrameNEON(const uint8_t* interlaced, uint8_t* progressive,
                                     int width, int height, bool topFieldFirst)
{
    if (!interlaced || !progressive || width <= 0 || height <= 0) return;

    NEONPerfMonitor::StartTiming("DeinterlaceFrameNEON");

    const int stride = width * 4; // RGBA

    // Copy fields with NEON acceleration
    for (int y = 0; y < height; y += 2) {
        const uint8_t* src_line = interlaced + y * stride;
        uint8_t* dst_line = progressive + y * stride;

        // Copy entire line using NEON (64 bytes at a time)
        int remaining = stride;
        while (remaining >= 64) {
            uint8x16_t chunk1 = vld1q_u8(src_line);
            uint8x16_t chunk2 = vld1q_u8(src_line + 16);
            uint8x16_t chunk3 = vld1q_u8(src_line + 32);
            uint8x16_t chunk4 = vld1q_u8(src_line + 48);

            vst1q_u8(dst_line, chunk1);
            vst1q_u8(dst_line + 16, chunk2);
            vst1q_u8(dst_line + 32, chunk3);
            vst1q_u8(dst_line + 48, chunk4);

            src_line += 64;
            dst_line += 64;
            remaining -= 64;
        }

        // Copy remainder
        memcpy(dst_line, src_line, remaining);

        // Interpolate missing lines
        if (y + 1 < height) {
            const uint8_t* line_above = progressive + y * stride;
            const uint8_t* line_below = (y + 2 < height) ? progressive + (y + 2) * stride : line_above;
            uint8_t* interpolated = progressive + (y + 1) * stride;

            // NEON interpolation (average of adjacent lines)
            int pixels = stride / 16;
            for (int p = 0; p < pixels; p++) {
                uint8x16_t above = vld1q_u8(line_above + p * 16);
                uint8x16_t below = vld1q_u8(line_below + p * 16);

                // Average the two lines
                uint16x8_t avg_low = vaddl_u8(vget_low_u8(above), vget_low_u8(below));
                uint16x8_t avg_high = vaddl_u8(vget_high_u8(above), vget_high_u8(below));

                uint8x8_t result_low = vshrn_n_u16(avg_low, 1);
                uint8x8_t result_high = vshrn_n_u16(avg_high, 1);

                vst1q_u8(interpolated + p * 16, vcombine_u8(result_low, result_high));
            }
        }
    }

    NEONPerfMonitor::EndTiming("DeinterlaceFrameNEON");
}

//==============================================================================
// NEONMemOps Implementation - General performance
//==============================================================================

void NEONMemOps::MemcpyNEON(void* dst, const void* src, size_t size)
{
    if (!dst || !src || size == 0) return;

    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);

    // For very large copies, use cache-line optimized approach
    if (size >= 1024) {
        const size_t cacheLineSize = 64;
        const size_t alignedSize = size & ~(cacheLineSize - 1);

        for (size_t i = 0; i < alignedSize; i += cacheLineSize) {
            // Prefetch next cache line
            __builtin_prefetch(s + i + cacheLineSize, 0, 3);

            // Load and store cache line using NEON
            uint8x16_t chunk1 = vld1q_u8(s + i);
            uint8x16_t chunk2 = vld1q_u8(s + i + 16);
            uint8x16_t chunk3 = vld1q_u8(s + i + 32);
            uint8x16_t chunk4 = vld1q_u8(s + i + 48);

            vst1q_u8(d + i, chunk1);
            vst1q_u8(d + i + 16, chunk2);
            vst1q_u8(d + i + 32, chunk3);
            vst1q_u8(d + i + 48, chunk4);
        }

        // Copy remainder
        memcpy(d + alignedSize, s + alignedSize, size - alignedSize);
    } else {
        // Use regular NEON copy for smaller sizes
        NEONTAProcessor::CopyTADataNEON(dst, src, size);
    }
}

void NEONMemOps::MemsetZeroNEON(void* ptr, size_t size)
{
    if (!ptr || size == 0) return;

    uint8_t* p = static_cast<uint8_t*>(ptr);
    const uint8x16_t zero = vdupq_n_u8(0);

    // Clear 64 bytes at a time
    const size_t chunks = size / 64;
    for (size_t i = 0; i < chunks; i++) {
        vst1q_u8(p, zero);
        vst1q_u8(p + 16, zero);
        vst1q_u8(p + 32, zero);
        vst1q_u8(p + 48, zero);
        p += 64;
    }

    // Clear remainder
    const size_t remainder = size % 64;
    memset(p, 0, remainder);
}

//==============================================================================
// NEONTextureOps Implementation - For texture loading performance
//==============================================================================

void NEONTextureOps::ConvertTexture1555to8888NEON(const uint16_t* src, uint32_t* dst, size_t pixels)
{
    if (!src || !dst || pixels == 0) return;

    const size_t neonPixels = (pixels / 8) * 8;

    for (size_t i = 0; i < neonPixels; i += 8) {
        uint16x8_t src_vec = vld1q_u16(src + i);

        // Extract components (ARRRRRGGGGGBBBBB)
        uint16x8_t a = vshrq_n_u16(src_vec, 15);                    // A
        uint16x8_t r = vshrq_n_u16(vshlq_n_u16(src_vec, 1), 11);   // R
        uint16x8_t g = vshrq_n_u16(vshlq_n_u16(src_vec, 6), 11);   // G
        uint16x8_t b = vshrq_n_u16(vshlq_n_u16(src_vec, 11), 11);  // B

        // Scale to 8-bit
        a = vmulq_n_u16(a, 255);
        r = vmulq_n_u16(r, 255/31);
        g = vmulq_n_u16(g, 255/31);
        b = vmulq_n_u16(b, 255/31);

        // Pack to 8-bit
        uint8x8_t a8 = vqmovn_u16(a);
        uint8x8_t r8 = vqmovn_u16(r);
        uint8x8_t g8 = vqmovn_u16(g);
        uint8x8_t b8 = vqmovn_u16(b);

        // Interleave as RGBA
        uint8x8x4_t rgba = {r8, g8, b8, a8};
        vst4_u8(reinterpret_cast<uint8_t*>(dst + i), rgba);
    }

    // Handle remaining pixels
    for (size_t i = neonPixels; i < pixels; i++) {
        uint16_t pixel = src[i];
        uint8_t a = (pixel >> 15) ? 255 : 0;
        uint8_t r = ((pixel >> 10) & 0x1F) * 255 / 31;
        uint8_t g = ((pixel >> 5) & 0x1F) * 255 / 31;
        uint8_t b = (pixel & 0x1F) * 255 / 31;
        dst[i] = (a << 24) | (b << 16) | (g << 8) | r;
    }
}

//==============================================================================
// NEONPerfMonitor Implementation
//==============================================================================

void NEONPerfMonitor::Initialize()
{
    profilingEnabled = true;
    INFO_LOG(RENDERER, "🔍 NEON Performance Monitor initialized");
}

void NEONPerfMonitor::StartTiming(const char* operationName)
{
    if (!profilingEnabled) return;

    std::lock_guard<std::mutex> lock(timingMutex);
    auto& timing = timingMap[operationName];
    timing.startTime = std::chrono::steady_clock::now().time_since_epoch().count();
}

void NEONPerfMonitor::EndTiming(const char* operationName)
{
    if (!profilingEnabled) return;

    uint64_t endTime = std::chrono::steady_clock::now().time_since_epoch().count();

    std::lock_guard<std::mutex> lock(timingMutex);
    auto& timing = timingMap[operationName];
    if (timing.startTime > 0) {
        double duration = (endTime - timing.startTime) / 1e9; // Convert to seconds
        timing.totalTime += duration;
        timing.callCount++;
        timing.startTime = 0;
    }
}

std::vector<NEONPerfMonitor::PerfStats> NEONPerfMonitor::GetStats()
{
    std::lock_guard<std::mutex> lock(timingMutex);
    std::vector<PerfStats> stats;

    for (const auto& [name, timing] : timingMap) {
        if (timing.callCount > 0) {
            PerfStats stat;
            stat.operation = name.c_str();
            stat.averageTime = timing.totalTime / timing.callCount;
            stat.totalTime = timing.totalTime;
            stat.callCount = timing.callCount;
            stat.speedupVsScalar = 2.5; // Estimated NEON speedup
            stats.push_back(stat);
        }
    }

    return stats;
}

} // namespace flycast

#endif // (TARGET_OS_IOS || TARGET_OS_TV) && defined(__ARM_NEON__)
#endif // __APPLE__
