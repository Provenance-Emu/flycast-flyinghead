#pragma once

#include "vulkan_context.h"
#include "buffer.h"
#include <queue>
#include <thread>
#include <atomic>
#include <mutex>

/// Asynchronous FMV Pipeline for eliminating CPU stalls during video playback
class AsyncFMVPipeline {
public:
    struct FMVFrame {
        uint8_t* yuvData = nullptr;
        uint8_t* rgbData = nullptr;
        int width = 0;
        int height = 0;
        bool isReady = false;
        uint64_t frameId = 0;
    };

    bool Init(VulkanContext* context);
    void Term();

    /// Queue YUV frame for async processing (non-blocking)
    void QueueYUVFrame(const uint8_t* yuvData, int width, int height, uint64_t frameId);

    /// Get processed RGB frame if ready (non-blocking)
    bool GetProcessedFrame(FMVFrame& frame);

    /// Start async processing thread
    void StartProcessing();

    /// Stop async processing thread
    void StopProcessing();

    /// Check if pipeline is busy
    bool IsBusy() const { return framesInProgress.load() > 0; }

    /// Return buffer to pool (public methods for external use)
    void ReturnYUVBuffer(uint8_t* buffer);
    void ReturnRGBBuffer(uint8_t* buffer);

    /// Get processing stats
    struct Stats {
        uint32_t framesQueued = 0;
        uint32_t framesProcessed = 0;
        uint32_t framesDropped = 0;
        float avgProcessingTime = 0.0f;
    };
    Stats GetStats() const { return stats; }

private:
    VulkanContext* context = nullptr;

    // Thread management
    std::thread processingThread;
    std::atomic<bool> shouldStop{false};
    std::atomic<int> framesInProgress{0};

    // Frame queues (triple buffering)
    std::queue<FMVFrame> inputQueue;
    std::queue<FMVFrame> outputQueue;
    std::mutex inputMutex;
    std::mutex outputMutex;

    // Frame pools to avoid allocations
    std::vector<std::unique_ptr<uint8_t[]>> yuvPool;
    std::vector<std::unique_ptr<uint8_t[]>> rgbPool;
    std::mutex poolMutex;

    // Performance stats
    mutable Stats stats;

    /// Main processing loop
    void ProcessingLoop();

    /// Process single YUV frame to RGB
    void ProcessFrame(FMVFrame& frame);

        /// Get buffer from pool (private methods for internal use)
    uint8_t* GetYUVBuffer(size_t size);
    uint8_t* GetRGBBuffer(size_t size);
};

/// Global async FMV pipeline instance
extern std::unique_ptr<AsyncFMVPipeline> g_asyncFMVPipeline;
