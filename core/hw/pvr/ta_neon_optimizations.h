/*
 * NEON-Optimized Tile Accelerator Processing for iOS/tvOS
 * Accelerates scene loading and FMV command processing
 */
#pragma once

#ifdef __APPLE__
#if (TARGET_OS_IOS || TARGET_OS_TV) && defined(__ARM_NEON__)

#include <arm_neon.h>
#include "ta.h"
#include "ta_ctx.h"

namespace flycast {

/// NEON-optimized TA command processing for improved scene loading performance
class NEONTAProcessor {
public:
    /// Initialize NEON-optimized TA processing
    static void Initialize();

    /// Process TA commands with NEON optimizations
    static void ProcessTACommandsNEON(const uint8_t* commands, size_t size);

    /// Optimized vertex data processing
    static void ProcessVertexDataNEON(const SQBuffer* data, size_t count);

    /// Fast polygon parameter processing
    static void ProcessPolygonParamsNEON(const PolyParam* params, size_t count);

    /// Accelerated vertex transformation for scene setup
    static void TransformVerticesNEON(const Vertex* input, Vertex* output,
                                     size_t count, const glm::mat4& matrix);

    /// Optimized texture coordinate generation
    static void GenerateTexCoordsNEON(const Vertex* vertices, size_t count,
                                     float* texCoords, float scaleU, float scaleV);

    /// Fast memory copy for TA data with prefetching
    static void CopyTADataNEON(void* dst, const void* src, size_t size);

    /// SIMD polygon sorting for improved rendering order
    static void SortPolygonsNEON(SortedTriangle* triangles, size_t count);

private:
    static bool isInitialized;
    static bool hasAdvancedNEON; // ARMv8.1+ features
};

/// NEON-optimized vertex operations for 3D scenes
class NEONVertexOps {
public:
    /// Transform vertices with 4x4 matrix using NEON
    static void Transform4x4NEON(const float* vertices, float* output,
                                size_t count, const float* matrix);

    /// Normalize vertex normals using NEON
    static void NormalizeNormalsNEON(float* normals, size_t count);

    /// Calculate vertex lighting using NEON
    static void CalculateLightingNEON(const Vertex* vertices, size_t count,
                                     const glm::vec3& lightDir, float* output);

    /// Clip vertices against frustum planes
    static size_t ClipVerticesNEON(const Vertex* input, Vertex* output,
                                  size_t count, const float* frustumPlanes);

    /// Interpolate vertex attributes during clipping
    static void InterpolateVertexNEON(const Vertex& v1, const Vertex& v2,
                                     float t, Vertex& result);
};

/// NEON-optimized texture operations for faster loading
class NEONTextureOps {
public:
    /// Convert texture formats using NEON
    static void ConvertTexture1555to8888NEON(const uint16_t* src, uint32_t* dst, size_t pixels);
    static void ConvertTexture565to8888NEON(const uint16_t* src, uint32_t* dst, size_t pixels);
    static void ConvertTexture4444to8888NEON(const uint16_t* src, uint32_t* dst, size_t pixels);

    /// Generate mipmaps using NEON box filtering
    static void GenerateMipmapBoxFilterNEON(const uint8_t* src, uint8_t* dst,
                                           int srcWidth, int srcHeight,
                                           int channels);

    /// Apply texture swizzling for optimal GPU access
    static void SwizzleTextureNEON(const uint8_t* src, uint8_t* dst,
                                  int width, int height, int channels);

    /// Compress texture data for memory efficiency
    static size_t CompressTextureNEON(const uint8_t* src, uint8_t* dst,
                                     int width, int height, int channels);
};

/// NEON-optimized memory operations for scene data
class NEONMemOps {
public:
    /// Ultra-fast memory copy with cache line optimization
    static void MemcpyNEON(void* dst, const void* src, size_t size);

    /// Memory copy with format conversion
    static void MemcpyConvertNEON(void* dst, const void* src, size_t elements,
                                 int srcFormat, int dstFormat);

    /// Zero memory using NEON stores
    static void MemsetZeroNEON(void* ptr, size_t size);

    /// Compare memory blocks using NEON
    static bool MemcmpNEON(const void* ptr1, const void* ptr2, size_t size);

    /// Checksum calculation for data validation
    static uint32_t CalculateChecksumNEON(const void* data, size_t size);
};

/// Scene-specific optimizations for loading performance
class NEONSceneOps {
public:
    /// Process scene geometry data in batches
    static void ProcessSceneGeometryNEON(const rend_context& ctx);

    /// Optimize polygon lists for rendering
    static void OptimizePolygonListsNEON(std::vector<PolyParam>& polygons);

    /// Cull polygons outside view frustum
    static void CullPolygonsNEON(const std::vector<PolyParam>& input,
                                std::vector<PolyParam>& output,
                                const glm::mat4& viewMatrix);

    /// Sort polygons by material for batching
    static void SortPolygonsByMaterialNEON(std::vector<PolyParam>& polygons);

    /// Generate level-of-detail for distant objects
    static void GenerateLODNEON(const std::vector<Vertex>& highLOD,
                               std::vector<Vertex>& lowLOD,
                               float simplificationRatio);
};

/// FMV-specific optimizations for smooth video playback
class NEONFMVOps {
public:
    /// Process YUV video data using NEON
    static void ProcessYUVFrameNEON(const uint8_t* yuvData, uint8_t* rgbData,
                                   int width, int height, int format);

    /// Deinterlace video frames for progressive display
    static void DeinterlaceFrameNEON(const uint8_t* interlaced, uint8_t* progressive,
                                    int width, int height, bool topFieldFirst);

    /// Apply color space conversion for FMV
    static void ConvertColorSpaceNEON(const uint8_t* input, uint8_t* output,
                                     int pixels, int srcSpace, int dstSpace);

    /// Temporal noise reduction for video quality
    static void ReduceVideoNoiseNEON(const uint8_t* current, const uint8_t* previous,
                                    uint8_t* output, int pixels, float threshold);

    /// Motion-adaptive deinterlacing
    static void MotionAdaptiveDeinterlaceNEON(const uint8_t* field1,
                                             const uint8_t* field2,
                                             uint8_t* output,
                                             int width, int height);
};

/// Performance monitoring for NEON optimizations
class NEONPerfMonitor {
public:
    /// Initialize performance monitoring
    static void Initialize();

    /// Start timing a NEON operation
    static void StartTiming(const char* operationName);

    /// End timing and record results
    static void EndTiming(const char* operationName);

    /// Get performance statistics
    struct PerfStats {
        const char* operation;
        double averageTime;
        double totalTime;
        uint64_t callCount;
        double speedupVsScalar; // Estimated speedup over scalar implementation
    };

    static std::vector<PerfStats> GetStats();

    /// Reset all performance counters
    static void ResetStats();

    /// Enable/disable detailed profiling
    static void SetProfilingEnabled(bool enabled);

private:
    struct TimingData {
        uint64_t startTime;
        double totalTime;
        uint64_t callCount;
    };

    static std::unordered_map<std::string, TimingData> timingMap;
    static std::mutex timingMutex;
    static bool profilingEnabled;
};

}

#endif // (TARGET_OS_IOS || TARGET_OS_TV) && defined(__ARM_NEON__)
#endif // __APPLE__
