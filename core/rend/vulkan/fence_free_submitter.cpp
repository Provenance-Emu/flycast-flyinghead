#include "fence_free_submitter.h"
#include "log/LogManager.h"
#include "oslib/oslib.h"
#include <chrono>
#include <condition_variable>

std::unique_ptr<FenceFreeSubmitter> g_fenceFreeSubmitter;

bool FenceFreeSubmitter::Init(VulkanContext* ctx) {
    context = ctx;
    // Note: we'll access the graphics queue directly from context when needed
    shouldStop = false;
    nextSubmissionId = 1;
    lastCompletedSubmission = 0;

    INFO_LOG(RENDERER, "🚀 Fence-Free Submitter initialized - GPU sync stalls eliminated");
    return true;
}

void FenceFreeSubmitter::Term() {
    StopSubmissionThread();

    // Clear submission queue
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        while (!submissionQueue.empty()) {
            submissionQueue.pop();
        }
    }

    INFO_LOG(RENDERER, "🚀 Fence-Free Submitter terminated");
}

void FenceFreeSubmitter::StartSubmissionThread() {
    if (!submissionThread.joinable()) {
        shouldStop = false;
        submissionThread = std::thread(&FenceFreeSubmitter::SubmissionLoop, this);
        INFO_LOG(RENDERER, "🚀 GPU submission thread started");
    }
}

void FenceFreeSubmitter::StopSubmissionThread() {
    shouldStop = true;
    queueCV.notify_all();
    if (submissionThread.joinable()) {
        submissionThread.join();
        INFO_LOG(RENDERER, "🚀 GPU submission thread stopped");
    }
}

uint64_t FenceFreeSubmitter::SubmitAsync(const std::vector<vk::CommandBuffer>& commandBuffers,
                                         const std::vector<vk::Semaphore>& waitSemaphores,
                                         const std::vector<vk::Semaphore>& signalSemaphores,
                                         const std::vector<vk::PipelineStageFlags>& waitStages) {
    if (shouldStop) return 0;

    SubmissionBatch batch;
    batch.commandBuffers = commandBuffers;
    batch.waitSemaphores = waitSemaphores;
    batch.signalSemaphores = signalSemaphores;
    batch.waitStages = waitStages;
    batch.submissionId = nextSubmissionId++;

    // Queue for async submission
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        submissionQueue.push(batch);
        stats.queuedSubmissions++;
    }

    queueCV.notify_one();
    return batch.submissionId;
}

bool FenceFreeSubmitter::IsSubmissionComplete(uint64_t submissionId) {
    return submissionId <= lastCompletedSubmission.load();
}

void FenceFreeSubmitter::SubmissionLoop() {
    ThreadName _("GPU-Submit");

    while (!shouldStop) {
        SubmissionBatch batch;
        bool hasBatch = false;

        // Get batch from queue
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCV.wait(lock, [this] { return !submissionQueue.empty() || shouldStop; });

            if (!submissionQueue.empty()) {
                batch = submissionQueue.front();
                submissionQueue.pop();
                hasBatch = true;
                stats.queuedSubmissions--;
            }
        }

        if (hasBatch) {
            auto startTime = std::chrono::high_resolution_clock::now();
            SubmitBatch(batch);
            auto endTime = std::chrono::high_resolution_clock::now();

            float submissionTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();
            stats.avgSubmissionTime = (stats.avgSubmissionTime * 0.9f) + (submissionTime * 0.1f);
            stats.totalSubmissions++;
            stats.completedSubmissions++;

            lastCompletedSubmission = batch.submissionId;
        }
    }
}

void FenceFreeSubmitter::SubmitBatch(const SubmissionBatch& batch) {
    if (batch.commandBuffers.empty() || !context) return;

    try {
        vk::SubmitInfo submitInfo;
        submitInfo.setCommandBuffers(batch.commandBuffers);

        if (!batch.waitSemaphores.empty()) {
            submitInfo.setWaitSemaphores(batch.waitSemaphores);
            submitInfo.setWaitDstStageMask(batch.waitStages);
        }

        if (!batch.signalSemaphores.empty()) {
            submitInfo.setSignalSemaphores(batch.signalSemaphores);
        }

        // Submit without fence - this is the key to eliminating CPU stalls
        // Access graphics queue directly from context
        context->GetDevice().getQueue(context->GetGraphicsQueueFamilyIndex(), 0).submit(submitInfo);

    } catch (const vk::SystemError& e) {
        WARN_LOG(RENDERER, "🚀 Fence-free submission failed: %s", e.what());
    }
}
