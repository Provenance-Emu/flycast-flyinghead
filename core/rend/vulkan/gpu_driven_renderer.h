#pragma once

#include "vulkan_context.h"
#include "buffer.h"
#include "hw/pvr/ta_ctx.h"
#include <glm/glm.hpp>

/// GPU-Driven Rendering System for Flycast
/// Implements compute-based culling and indirect drawing for mobile optimization
class GPUDrivenRenderer
{
public:
    struct ObjectData
    {
        glm::mat4 mvpMatrix;
        glm::vec3 center;
        float radius;
        uint32_t materialId;
        uint32_t geometryOffset;
        uint32_t indexCount;
        uint32_t flags; // transparent, modvol, etc.
    };

    struct TileData
    {
        glm::vec4 bounds;     // minX, minY, maxX, maxY
        uint32_t objectOffset;
        uint32_t objectCount;
        uint32_t sortKey;
        uint32_t reserved;
    };

    struct DrawCommand
    {
        uint32_t indexCount;
        uint32_t instanceCount;
        uint32_t firstIndex;
        int32_t vertexOffset;
        uint32_t firstInstance;
    };

    struct CullingUniforms
    {
        glm::mat4 viewMatrix;
        glm::mat4 projMatrix;
        glm::vec4 frustumPlanes[6];
        glm::vec2 screenSize;
        uint32_t maxObjects;
        uint32_t tileCount;
    };

    struct GPUStats
    {
        uint32_t totalObjects;
        uint32_t visibleObjects;
        uint32_t culledObjects;
        uint32_t drawCalls;
    };

    bool Init(VulkanContext* context, uint32_t maxObjects = 16384);
    void Term();

    /// Upload scene objects for GPU culling
    void UploadObjects(const std::vector<ObjectData>& objects);

    /// Perform GPU culling and generate draw commands
    void PerformCulling(const CullingUniforms& uniforms);

    /// Execute indirect draws
    void ExecuteIndirectDraws(vk::CommandBuffer cmdBuffer, vk::Pipeline pipeline);

    /// Get GPU performance stats
    GPUStats GetStats() const { return stats; }

    /// Check if GPU-driven rendering is enabled
    bool IsEnabled() const { return enabled && initialized; }

    /// Enable/disable GPU-driven rendering
    void SetEnabled(bool enable) { enabled = enable; }

private:
    VulkanContext* context = nullptr;
    bool initialized = false;
    bool enabled = true;

    /// iOS device tier for optimization levels
    enum class DeviceTier {
        Low,      // < 2GB RAM, conservative settings
        Medium,   // 2-4GB RAM, balanced
        High      // > 4GB RAM, full optimization
    } deviceTier = DeviceTier::Medium;

    /// Compute shader resources
    vk::UniqueShaderModule cullingShader;
    vk::UniqueShaderModule tileBinningShader;
    vk::UniquePipelineLayout computePipelineLayout;
    vk::UniquePipeline cullingPipeline;
    vk::UniquePipeline tileBinningPipeline;
    vk::UniqueDescriptorSetLayout computeDescriptorLayout;
    vk::UniqueDescriptorPool computeDescriptorPool;
    vk::DescriptorSet computeDescriptorSet;

    /// GPU buffers
    std::unique_ptr<BufferData> objectBuffer;        // Input objects
    std::unique_ptr<BufferData> tileBuffer;          // Tile data
    std::unique_ptr<BufferData> indirectBuffer;      // Draw commands
    std::unique_ptr<BufferData> visibilityBuffer;    // Per-object visibility
    std::unique_ptr<BufferData> cullResultBuffer;    // Culling results
    std::unique_ptr<BufferData> uniformBuffer;       // Culling uniforms
    std::unique_ptr<BufferData> statsBuffer;         // GPU stats

    /// Configuration
    uint32_t maxObjects = 16384;
    uint32_t tileSize = 32;        // 32x32 pixel tiles for mobile
    uint32_t maxTiles = 1024;

    GPUStats stats;

    void CreateComputeShaders();
    void CreateBuffers();
    void CreateDescriptorSets();
    void DetectDeviceTier();

    /// Shader creation helpers
    vk::UniqueShaderModule CreateShaderModule(const std::vector<uint32_t>& spirv);
    std::vector<uint32_t> CompileCullingShader();
    std::vector<uint32_t> CompileTileBinningShader();
};

/// Global GPU-driven renderer instance
extern std::unique_ptr<GPUDrivenRenderer> g_gpuDrivenRenderer;
