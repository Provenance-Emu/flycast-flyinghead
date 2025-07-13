#include "fmv_async_pipeline.h"
#include "hw/pvr/ta_neon_optimizations.h"
#include "log/LogManager.h"
#include "oslib/oslib.h"
#include <chrono>
#include <algorithm>

std::unique_ptr<AsyncFMVPipeline> g_asyncFMVPipeline;

bool AsyncFMVPipeline::Init(VulkanContext* ctx) {
    context = ctx;
    shouldStop = false;
    framesInProgress = 0;

    // Pre-allocate buffer pools (triple buffering)
    for (int i = 0; i < 3; i++) {
        yuvPool.push_back(std::make_unique<uint8_t[]>(1920 * 1080 * 2)); // Max YUV size
        rgbPool.push_back(std::make_unique<uint8_t[]>(1920 * 1080 * 4)); // Max RGB size
    }

    INFO_LOG(RENDERER, "🎬 Async FMV Pipeline initialized - CPU stall elimination active");
    return true;
}

void AsyncFMVPipeline::Term() {
    StopProcessing();

    // Clear queues
    {
        std::lock_guard<std::mutex> lock(inputMutex);
        while (!inputQueue.empty()) {
            inputQueue.pop();
        }
    }
    {
        std::lock_guard<std::mutex> lock(outputMutex);
        while (!outputQueue.empty()) {
            outputQueue.pop();
        }
    }

    // Clear pools
    {
        std::lock_guard<std::mutex> lock(poolMutex);
        yuvPool.clear();
        rgbPool.clear();
    }

    INFO_LOG(RENDERER, "🎬 Async FMV Pipeline terminated");
}

void AsyncFMVPipeline::StartProcessing() {
    if (!processingThread.joinable()) {
        shouldStop = false;
        processingThread = std::thread(&AsyncFMVPipeline::ProcessingLoop, this);
        INFO_LOG(RENDERER, "🎬 FMV processing thread started");
    }
}

void AsyncFMVPipeline::StopProcessing() {
    shouldStop = true;
    if (processingThread.joinable()) {
        processingThread.join();
        INFO_LOG(RENDERER, "🎬 FMV processing thread stopped");
    }
}

void AsyncFMVPipeline::QueueYUVFrame(const uint8_t* yuvData, int width, int height, uint64_t frameId) {
    if (shouldStop) return;

    // Get buffer from pool
    uint8_t* yuvBuffer = GetYUVBuffer(width * height * 2);
    if (!yuvBuffer) {
        stats.framesDropped++;
        return;
    }

    // Copy YUV data (non-blocking)
    memcpy(yuvBuffer, yuvData, width * height * 2);

    FMVFrame frame;
    frame.yuvData = yuvBuffer;
    frame.width = width;
    frame.height = height;
    frame.frameId = frameId;
    frame.isReady = false;

    // Queue for processing
    {
        std::lock_guard<std::mutex> lock(inputMutex);
        inputQueue.push(frame);
        stats.framesQueued++;
    }

    framesInProgress++;
}

bool AsyncFMVPipeline::GetProcessedFrame(FMVFrame& frame) {
    std::lock_guard<std::mutex> lock(outputMutex);

    if (outputQueue.empty()) {
        return false;
    }

    frame = outputQueue.front();
    outputQueue.pop();
    framesInProgress--;

    return frame.isReady;
}

void AsyncFMVPipeline::ProcessingLoop() {
    ThreadName _("FMV-Async");

    while (!shouldStop) {
        FMVFrame frame;
        bool hasFrame = false;

        // Get frame from input queue
        {
            std::lock_guard<std::mutex> lock(inputMutex);
            if (!inputQueue.empty()) {
                frame = inputQueue.front();
                inputQueue.pop();
                hasFrame = true;
            }
        }

        if (hasFrame) {
            // Process frame using NEON optimizations
            auto startTime = std::chrono::high_resolution_clock::now();
            ProcessFrame(frame);
            auto endTime = std::chrono::high_resolution_clock::now();

            // Update timing stats
            float processingTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();
            stats.avgProcessingTime = (stats.avgProcessingTime * 0.9f) + (processingTime * 0.1f);
            stats.framesProcessed++;

            // Add to output queue
            {
                std::lock_guard<std::mutex> lock(outputMutex);
                outputQueue.push(frame);
            }
        } else {
            // No frames to process, yield CPU
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
}

void AsyncFMVPipeline::ProcessFrame(FMVFrame& frame) {
    // Get RGB buffer from pool
    frame.rgbData = GetRGBBuffer(frame.width * frame.height * 4);
    if (!frame.rgbData) {
        frame.isReady = false;
        return;
    }

#ifdef __APPLE__
#if TARGET_OS_IOS || TARGET_OS_TV
    // Use NEON optimizations for YUV→RGB conversion
    flycast::NEONFMVOps::ProcessYUVFrameNEON(
        frame.yuvData, frame.rgbData,
        frame.width, frame.height, 0
    );
#else
    // Fallback for other platforms
    // Basic YUV422→RGB conversion
    const uint8_t* yuv = frame.yuvData;
    uint8_t* rgb = frame.rgbData;

    for (int i = 0; i < frame.width * frame.height; i += 2) {
        uint8_t y1 = yuv[i * 2 + 1];
        uint8_t u  = yuv[i * 2 + 0];
        uint8_t y2 = yuv[i * 2 + 3];
        uint8_t v  = yuv[i * 2 + 2];

        // Convert YUV to RGB
        int c1 = y1 - 16;
        int c2 = y2 - 16;
        int d = u - 128;
        int e = v - 128;

        // Pixel 1
        rgb[i * 4 + 0] = std::clamp((298 * c1 + 409 * e + 128) >> 8, 0, 255);
        rgb[i * 4 + 1] = std::clamp((298 * c1 - 100 * d - 208 * e + 128) >> 8, 0, 255);
        rgb[i * 4 + 2] = std::clamp((298 * c1 + 516 * d + 128) >> 8, 0, 255);
        rgb[i * 4 + 3] = 255;

        // Pixel 2
        rgb[i * 4 + 4] = std::clamp((298 * c2 + 409 * e + 128) >> 8, 0, 255);
        rgb[i * 4 + 5] = std::clamp((298 * c2 - 100 * d - 208 * e + 128) >> 8, 0, 255);
        rgb[i * 4 + 6] = std::clamp((298 * c2 + 516 * d + 128) >> 8, 0, 255);
        rgb[i * 4 + 7] = 255;
    }
#endif
#endif

    frame.isReady = true;
}

uint8_t* AsyncFMVPipeline::GetYUVBuffer(size_t size) {
    std::lock_guard<std::mutex> lock(poolMutex);

    for (auto& buffer : yuvPool) {
        if (buffer) {
            uint8_t* ptr = buffer.release();
            return ptr;
        }
    }

    // Pool exhausted, create new buffer
    return new uint8_t[size];
}

uint8_t* AsyncFMVPipeline::GetRGBBuffer(size_t size) {
    std::lock_guard<std::mutex> lock(poolMutex);

    for (auto& buffer : rgbPool) {
        if (buffer) {
            uint8_t* ptr = buffer.release();
            return ptr;
        }
    }

    // Pool exhausted, create new buffer
    return new uint8_t[size];
}

void AsyncFMVPipeline::ReturnYUVBuffer(uint8_t* buffer) {
    if (!buffer) return;

    std::lock_guard<std::mutex> lock(poolMutex);
    yuvPool.push_back(std::unique_ptr<uint8_t[]>(buffer));
}

void AsyncFMVPipeline::ReturnRGBBuffer(uint8_t* buffer) {
    if (!buffer) return;

    std::lock_guard<std::mutex> lock(poolMutex);
    rgbPool.push_back(std::unique_ptr<uint8_t[]>(buffer));
}
