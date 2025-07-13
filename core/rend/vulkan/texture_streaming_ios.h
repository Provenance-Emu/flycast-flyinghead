/*
 * iOS/tvOS Texture Streaming Optimizations for Flycast
 * Optimizes FMV and scene loading performance using device-specific strategies
 */
#pragma once

#ifdef __APPLE__
#if TARGET_OS_IOS || TARGET_OS_TV

#include "texture.h"
#include <Accelerate/Accelerate.h>
#include <arm_neon.h>
#include <sys/sysctl.h>
#include <dispatch/dispatch.h>

namespace flycast {

/// iOS-specific texture streaming manager for optimal FMV performance
class IOSTextureStreamingManager {
public:
    enum class DevicePerformanceTier {
        LOW_PERFORMANCE,     // A9, A10 (older iPads, Apple TV 4K 1st gen)
        MEDIUM_PERFORMANCE,  // A11, A12 (iPhone X era, Apple TV 4K 2nd gen)
        HIGH_PERFORMANCE     // A13+ (modern devices)
    };

    enum class TextureFormat {
        RGBA8,
        YUV422,
        BGRA8
    };

    struct TextureData {
        void* data = nullptr;
        int width = 0;
        int height = 0;
        TextureFormat format = TextureFormat::RGBA8;
    };

    static IOSTextureStreamingManager& Instance() {
        static IOSTextureStreamingManager instance;
        return instance;
    }

    /// Initialize the streaming manager with device-specific optimizations
    void Initialize();

    /// Optimize texture for current device capabilities
    void OptimizeTextureForDevice(TextureData& texture);

    /// Resize texture using NEON optimizations
    void ResizeTextureNEON(const uint8_t* srcData, uint8_t* dstData,
                          int srcWidth, int srcHeight,
                          int dstWidth, int dstHeight, int bytesPerPixel);

    /// Prefetch textures for upcoming scene
    void PrefetchTextures(const std::vector<uint32_t>& textureIds);

    /// Get current device performance tier
    DevicePerformanceTier GetDeviceTier() const { return deviceTier; }

private:
    /// Detect device capabilities and memory
    void DetectDeviceCapabilities();

    /// Setup memory strategy based on device tier
    void SetupOptimalMemoryStrategy();

    /// Convert YUV422 to RGB using Accelerate framework
    void ConvertYUVToRGBAccelerated(TextureData& texture);

    /// Upload texture asynchronously
    void UploadTextureAsync(const TextureData& texture);

    DevicePerformanceTier deviceTier = DevicePerformanceTier::MEDIUM_PERFORMANCE;
    uint64_t deviceMemory = 0;
    size_t optimalTexturePoolSize = 64 * 1024 * 1024; // 64MB default

    // Performance settings
    int maxConcurrentUploads = 2;
    bool useAsyncTextureUploads = true;
    bool preferLargerTextureTiles = false;

    dispatch_queue_t textureUploadQueue = nullptr;
};

/// Fast memory operations using NEON instructions
class FastMemoryOperations {
public:
    /// Copy texture data using NEON optimization (64-byte chunks)
    static void CopyTextureDataNEON(const void* src, void* dst, size_t size);

    /// Zero texture memory using NEON optimization
    static void ZeroTextureMemoryNEON(void* ptr, size_t size);

    /// Prefetch memory range for better cache performance
    static void PrefetchMemoryRange(const void* ptr, size_t size);
};

} // namespace flycast

#endif // TARGET_OS_IOS || TARGET_OS_TV
#endif // __APPLE__
