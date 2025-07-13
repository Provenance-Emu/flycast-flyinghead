#pragma once

#include "vulkan_context.h"
#include <vector>
#include <queue>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

/// Fence-free command submission system to eliminate CPU-GPU synchronization stalls
class FenceFreeSubmitter {
public:
    struct SubmissionBatch {
        std::vector<vk::CommandBuffer> commandBuffers;
        std::vector<vk::Semaphore> waitSemaphores;
        std::vector<vk::Semaphore> signalSemaphores;
        std::vector<vk::PipelineStageFlags> waitStages;
        uint64_t submissionId = 0;
    };

    bool Init(VulkanContext* context);
    void Term();

    /// Submit command buffer without blocking (fence-free)
    uint64_t SubmitAsync(const std::vector<vk::CommandBuffer>& commandBuffers,
                        const std::vector<vk::Semaphore>& waitSemaphores = {},
                        const std::vector<vk::Semaphore>& signalSemaphores = {},
                        const std::vector<vk::PipelineStageFlags>& waitStages = {});

    /// Check if submission is complete (non-blocking)
    bool IsSubmissionComplete(uint64_t submissionId);

    /// Get completion status of all submissions
    uint64_t GetLastCompletedSubmission() const { return lastCompletedSubmission.load(); }

    /// Start async submission thread
    void StartSubmissionThread();

    /// Stop async submission thread
    void StopSubmissionThread();

    /// Performance stats
    struct Stats {
        uint32_t totalSubmissions = 0;
        uint32_t completedSubmissions = 0;
        uint32_t queuedSubmissions = 0;
        float avgSubmissionTime = 0.0f;
    };
    Stats GetStats() const { return stats; }

private:
    VulkanContext* context = nullptr;

    // Thread management
    std::thread submissionThread;
    std::atomic<bool> shouldStop{false};

    // Submission tracking
    std::atomic<uint64_t> nextSubmissionId{1};
    std::atomic<uint64_t> lastCompletedSubmission{0};

    // Submission queue
    std::queue<SubmissionBatch> submissionQueue;
    std::mutex queueMutex;
    std::condition_variable queueCV;

    // Performance stats
    mutable Stats stats;

    /// Main submission loop
    void SubmissionLoop();

    /// Submit single batch
    void SubmitBatch(const SubmissionBatch& batch);
};

/// Global fence-free submitter instance
extern std::unique_ptr<FenceFreeSubmitter> g_fenceFreeSubmitter;
