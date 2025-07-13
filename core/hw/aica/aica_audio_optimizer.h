/*
 * AICA Audio Optimizer for iOS Performance
 * Lock-free threaded audio processing with ring buffers
 */

#pragma once
#include "types.h"
#include "sgc_if.h"
#include <atomic>
#include <thread>
#include <memory>

// Use the SampleType from the sgc namespace
using SampleType = aica::sgc::SampleType;

namespace aica::audio_optimizer {

/// Lock-free ring buffer for audio samples
template<typename T, size_t Size>
class LockFreeRingBuffer {
public:
    LockFreeRingBuffer() : head(0), tail(0) {}

    bool push(const T& item) {
        const auto current_tail = tail.load(std::memory_order_relaxed);
        const auto next_tail = increment(current_tail);
        if (next_tail != head.load(std::memory_order_acquire)) {
            buffer[current_tail] = item;
            tail.store(next_tail, std::memory_order_release);
            return true;
        }
        return false; // Buffer full
    }

    bool pop(T& item) {
        const auto current_head = head.load(std::memory_order_relaxed);
        if (current_head == tail.load(std::memory_order_acquire)) {
            return false; // Buffer empty
        }
        item = buffer[current_head];
        head.store(increment(current_head), std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head.load(std::memory_order_acquire) == tail.load(std::memory_order_acquire);
    }

    size_t size() const {
        const auto current_head = head.load(std::memory_order_acquire);
        const auto current_tail = tail.load(std::memory_order_acquire);
        if (current_tail >= current_head) {
            return current_tail - current_head;
        } else {
            return Size - current_head + current_tail;
        }
    }

private:
    static constexpr size_t increment(size_t idx) {
        return (idx + 1) % Size;
    }

    T buffer[Size];
    std::atomic<size_t> head;
    std::atomic<size_t> tail;
};

/// Audio processing request structure
struct AudioProcessingRequest {
    SampleType* output_left;
    SampleType* output_right;
    bool completed;
    bool ready;

    AudioProcessingRequest() : output_left(nullptr), output_right(nullptr), completed(false), ready(false) {}
};

/// Threaded audio performance optimizer for iOS
class AICAAudioOptimizer {
public:
    /// Initialize audio optimizations
    static void Init();

    /// Terminate audio optimizations
    static void Term();

    /// Check if optimizations are enabled
    static bool IsEnabled() { return enabled; }

    /// Check if threaded processing is active
    static bool IsThreadedMode() { return threaded_mode; }

    /// Process audio channels (main thread entry point)
    static void ProcessChannelsOptimized(SampleType& mixl, SampleType& mixr);

    /// Queue audio processing request (non-blocking)
    static bool QueueAudioProcessing(SampleType& mixl, SampleType& mixr);

    /// Audio processing statistics
    struct ThreadedAudioStats {
        std::atomic<uint64_t> requests_queued{0};
        std::atomic<uint64_t> requests_processed{0};
        std::atomic<uint64_t> buffer_overruns{0};
        std::atomic<double> avg_processing_time_us{0.0};
        std::atomic<double> thread_utilization{0.0};
        std::atomic<bool> thread_active{false};
    };

        static ThreadedAudioStats& GetThreadedStats() { return threaded_stats; }

    /// Get thread utilization for external access
    static double GetThreadUtilization() { return threaded_stats.thread_utilization.load(); }

private:
    static bool enabled;
    static bool threaded_mode;

    // Threading components
    static std::unique_ptr<std::thread> audio_thread;
    static std::atomic<bool> should_terminate;
    static std::atomic<bool> thread_running;

    // Lock-free buffers
    static constexpr size_t RING_BUFFER_SIZE = 1024;
    static LockFreeRingBuffer<AudioProcessingRequest, RING_BUFFER_SIZE> request_queue;
    static LockFreeRingBuffer<std::pair<SampleType, SampleType>, RING_BUFFER_SIZE> result_queue;

    // Audio buffer pool for lock-free operation
    static constexpr size_t BUFFER_POOL_SIZE = 16;
    static SampleType buffer_pool_left[BUFFER_POOL_SIZE];
    static SampleType buffer_pool_right[BUFFER_POOL_SIZE];
    static std::atomic<int> next_buffer_index;

    // Performance tracking
    static ThreadedAudioStats threaded_stats;

    /// Audio processing thread main function
    static void AudioProcessingThreadMain();

    /// Fast channel processing with reduced function call overhead
    static void ProcessChannelsFast(SampleType& mixl, SampleType& mixr);

    /// Get next available buffer from pool
    static std::pair<SampleType*, SampleType*> GetBufferPair();

    /// Process queued audio on main thread (fallback)
    static void ProcessQueuedAudio();

    /// Initialize threading components
    static bool InitThreading();

    /// Terminate threading components
    static void TermThreading();
};

/// Audio processing statistics for performance monitoring
struct AICAAudioStats {
    uint64_t channels_processed;
    double avg_processing_time_ms;
    double cpu_utilization_percent;
    bool optimizations_active;
    bool threaded_processing_active;
    uint64_t lock_free_operations;
    double thread_efficiency_percent;
};

/// Get current audio optimization statistics
AICAAudioStats GetAudioOptimizationStats();

/// Reset audio optimization statistics
void ResetAudioOptimizationStats();

} // namespace aica::audio_optimizer
