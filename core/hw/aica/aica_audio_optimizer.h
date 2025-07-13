/*
 * AICA Audio Optimizer for iOS Performance
 * Targeted optimizations for FMV audio processing bottlenecks
 */

#pragma once
#include "types.h"
#include "sgc_if.h"

// Use the SampleType from the sgc namespace
using SampleType = aica::sgc::SampleType;

namespace aica::audio_optimizer {

/// Simple audio performance optimizer for iOS
class AICAAudioOptimizer {
public:
    /// Initialize audio optimizations
    static void Init();

    /// Terminate audio optimizations
    static void Term();

    /// Check if optimizations are enabled
    static bool IsEnabled() { return enabled; }

    /// Optimized multi-channel audio processing (replaces ChannelEx::StepAll)
    static void ProcessChannelsOptimized(SampleType& mixl, SampleType& mixr);

private:
    static bool enabled;

    /// Fast channel processing with reduced function call overhead
    static void ProcessChannelsFast(SampleType& mixl, SampleType& mixr);
};

/// Audio processing statistics for performance monitoring
struct AICAAudioStats {
    uint64_t channels_processed;
    double avg_processing_time_ms;
    double cpu_utilization_percent;
    bool optimizations_active;
};

/// Get current audio optimization statistics
AICAAudioStats GetAudioOptimizationStats();

/// Reset audio optimization statistics
void ResetAudioOptimizationStats();

} // namespace aica::audio_optimizer
