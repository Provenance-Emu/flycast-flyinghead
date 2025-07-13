#include "gpu_driven_renderer.h"
#include "cfg/option.h"
#include "log/LogManager.h"
#include <glm/gtc/matrix_transform.hpp>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

/// Global instance
std::unique_ptr<GPUDrivenRenderer> g_gpuDrivenRenderer;

/// GLSL compute shader source for frustum culling
static const char* CULLING_SHADER_SOURCE = R"(
#version 450
#extension GL_ARB_compute_variable_group_size : enable

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct ObjectData {
    mat4 mvpMatrix;
    vec3 center;
    float radius;
    uint materialId;
    uint geometryOffset;
    uint indexCount;
    uint flags;
};

struct DrawCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};

layout(std140, binding = 0) uniform CullingUniforms {
    mat4 viewMatrix;
    mat4 projMatrix;
    vec4 frustumPlanes[6];
    vec2 screenSize;
    uint maxObjects;
    uint tileCount;
} uniforms;

layout(std430, binding = 1) readonly buffer ObjectBuffer {
    ObjectData objects[];
};

layout(std430, binding = 2) writeonly buffer DrawCommandBuffer {
    DrawCommand drawCommands[];
};

layout(std430, binding = 3) writeonly buffer VisibilityBuffer {
    uint visibility[];
};

layout(std430, binding = 4) buffer StatsBuffer {
    uint totalObjects;
    uint visibleObjects;
    uint culledObjects;
    uint drawCalls;
} stats;

bool FrustumCullSphere(vec3 center, float radius) {
    for (int i = 0; i < 6; i++) {
        float dist = dot(uniforms.frustumPlanes[i].xyz, center) + uniforms.frustumPlanes[i].w;
        if (dist < -radius) {
            return true; // Outside frustum
        }
    }
    return false;
}

void main() {
    uint objectId = gl_GlobalInvocationID.x;

    if (objectId >= uniforms.maxObjects) {
        return;
    }

    ObjectData obj = objects[objectId];

    // Transform center to view space
    vec3 viewCenter = (uniforms.viewMatrix * vec4(obj.center, 1.0)).xyz;

    // Frustum culling
    bool visible = !FrustumCullSphere(viewCenter, obj.radius);

    // Distance culling for very small objects
    float distance = length(viewCenter);
    if (distance > 1000.0 && obj.radius < 0.1) {
        visible = false;
    }

    visibility[objectId] = visible ? 1u : 0u;

    if (visible) {
        uint drawId = atomicAdd(stats.visibleObjects, 1u);

        drawCommands[drawId].indexCount = obj.indexCount;
        drawCommands[drawId].instanceCount = 1u;
        drawCommands[drawId].firstIndex = obj.geometryOffset;
        drawCommands[drawId].vertexOffset = 0;
        drawCommands[drawId].firstInstance = objectId;
    } else {
        atomicAdd(stats.culledObjects, 1u);
    }
}
)";

/// GLSL compute shader for tile-based binning
static const char* TILE_BINNING_SHADER_SOURCE = R"(
#version 450
#extension GL_ARB_compute_variable_group_size : enable

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

struct ObjectData {
    mat4 mvpMatrix;
    vec3 center;
    float radius;
    uint materialId;
    uint geometryOffset;
    uint indexCount;
    uint flags;
};

struct TileData {
    vec4 bounds;
    uint objectOffset;
    uint objectCount;
    uint sortKey;
    uint reserved;
};

layout(std140, binding = 0) uniform CullingUniforms {
    mat4 viewMatrix;
    mat4 projMatrix;
    vec4 frustumPlanes[6];
    vec2 screenSize;
    uint maxObjects;
    uint tileCount;
} uniforms;

layout(std430, binding = 1) readonly buffer ObjectBuffer {
    ObjectData objects[];
};

layout(std430, binding = 2) readonly buffer VisibilityBuffer {
    uint visibility[];
};

layout(std430, binding = 3) buffer TileBuffer {
    TileData tiles[];
};

layout(std430, binding = 4) buffer TileObjectBuffer {
    uint tileObjects[];
};

shared uint sharedTileObjectCount;

bool ProjectSphereToTile(vec3 center, float radius, uvec2 tileId, uvec2 tileSize) {
    vec4 clipPos = uniforms.projMatrix * vec4(center, 1.0);
    vec3 ndc = clipPos.xyz / clipPos.w;

    // Convert to screen space
    vec2 screenPos = (ndc.xy * 0.5 + 0.5) * uniforms.screenSize;

    // Calculate sphere screen-space radius
    vec4 clipRadius = uniforms.projMatrix * vec4(center + vec3(radius, 0, 0), 1.0);
    vec3 ndcRadius = clipRadius.xyz / clipRadius.w;
    vec2 screenRadius = abs((ndcRadius.xy - ndc.xy) * 0.5) * uniforms.screenSize;
    float maxScreenRadius = max(screenRadius.x, screenRadius.y);

    // Calculate tile bounds
    vec2 tileMin = vec2(tileId) * vec2(tileSize);
    vec2 tileMax = tileMin + vec2(tileSize);

    // Check if sphere intersects tile
    vec2 closest = clamp(screenPos, tileMin, tileMax);
    vec2 distance = screenPos - closest;
    return length(distance) <= maxScreenRadius;
}

void main() {
    uvec2 tileId = gl_WorkGroupID.xy;
    uvec2 localId = gl_LocalInvocationID.xy;
    uvec2 tileSize = uvec2(32, 32); // 32x32 pixel tiles

    uint tileIndex = tileId.y * (uint(uniforms.screenSize.x) / tileSize.x) + tileId.x;

    if (tileIndex >= uniforms.tileCount) {
        return;
    }

    // Initialize shared memory
    if (localId.x == 0 && localId.y == 0) {
        sharedTileObjectCount = 0;
    }
    barrier();

    // Each thread processes multiple objects
    uint threadsPerTile = tileSize.x * tileSize.y;
    uint objectsPerThread = (uniforms.maxObjects + threadsPerTile - 1) / threadsPerTile;
    uint threadId = localId.y * tileSize.x + localId.x;
    uint startObject = threadId * objectsPerThread;

    for (uint i = 0; i < objectsPerThread; i++) {
        uint objectId = startObject + i;
        if (objectId >= uniforms.maxObjects) {
            break;
        }

        if (visibility[objectId] == 0u) {
            continue;
        }

        ObjectData obj = objects[objectId];
        vec3 viewCenter = (uniforms.viewMatrix * vec4(obj.center, 1.0)).xyz;

        if (ProjectSphereToTile(viewCenter, obj.radius, tileId, tileSize)) {
            uint localIndex = atomicAdd(sharedTileObjectCount, 1u);
            // Store in shared memory first, then write to global memory
            // This would need proper implementation with shared arrays
        }
    }

    barrier();

    // Write tile data
    if (localId.x == 0 && localId.y == 0) {
        tiles[tileIndex].bounds = vec4(vec2(tileId) * vec2(tileSize),
                                      (vec2(tileId) + vec2(1)) * vec2(tileSize));
        tiles[tileIndex].objectCount = sharedTileObjectCount;
        tiles[tileIndex].sortKey = tileIndex;
    }
}
)";

bool GPUDrivenRenderer::Init(VulkanContext* ctx, uint32_t maxObjs)
{
    context = ctx;
    maxObjects = maxObjs;

    INFO_LOG(RENDERER, "Initializing GPU-Driven Renderer (maxObjects: %u)", maxObjects);

    DetectDeviceTier();

    try {
        CreateComputeShaders();
        CreateBuffers();
        CreateDescriptorSets();

        initialized = true;

        INFO_LOG(RENDERER, "GPU-Driven Renderer initialized successfully (Device Tier: %s)",
                deviceTier == DeviceTier::High ? "High" :
                deviceTier == DeviceTier::Medium ? "Medium" : "Low");

        return true;
    }
    catch (const std::exception& e) {
        ERROR_LOG(RENDERER, "Failed to initialize GPU-Driven Renderer: %s", e.what());
        Term();
        return false;
    }
}

void GPUDrivenRenderer::Term()
{
    if (!initialized) {
        return;
    }

    if (context && context->GetDevice()) {
        context->WaitIdle();
    }

    // Reset all resources
    statsBuffer.reset();
    uniformBuffer.reset();
    cullResultBuffer.reset();
    visibilityBuffer.reset();
    indirectBuffer.reset();
    tileBuffer.reset();
    objectBuffer.reset();

    computeDescriptorPool.reset();
    computeDescriptorLayout.reset();
    tileBinningPipeline.reset();
    cullingPipeline.reset();
    computePipelineLayout.reset();
    tileBinningShader.reset();
    cullingShader.reset();

    initialized = false;
    INFO_LOG(RENDERER, "GPU-Driven Renderer terminated");
}

void GPUDrivenRenderer::DetectDeviceTier()
{
#ifdef __APPLE__
    size_t size = sizeof(uint64_t);
    uint64_t memSize = 0;

    if (sysctlbyname("hw.memsize", &memSize, &size, nullptr, 0) == 0) {
        uint64_t memGB = memSize / (1024 * 1024 * 1024);

        if (memGB < 2) {
            deviceTier = DeviceTier::Low;
            maxObjects = 8192;
            tileSize = 16;
            maxTiles = 512;
        } else if (memGB < 4) {
            deviceTier = DeviceTier::Medium;
            maxObjects = 16384;
            tileSize = 32;
            maxTiles = 1024;
        } else {
            deviceTier = DeviceTier::High;
            maxObjects = 32768;
            tileSize = 32;
            maxTiles = 2048;
        }

        INFO_LOG(RENDERER, "Detected iOS device memory: %lluGB, tier: %s",
                memGB,
                deviceTier == DeviceTier::High ? "High" :
                deviceTier == DeviceTier::Medium ? "Medium" : "Low");
    }
#endif
}

void GPUDrivenRenderer::CreateComputeShaders()
{
    // Create culling compute shader
    std::vector<uint32_t> cullingSpirv = CompileCullingShader();
    cullingShader = CreateShaderModule(cullingSpirv);

    // Create tile binning compute shader
    std::vector<uint32_t> tileBinningSpirv = CompileTileBinningShader();
    tileBinningShader = CreateShaderModule(tileBinningSpirv);

    // Create descriptor set layout
    std::array<vk::DescriptorSetLayoutBinding, 5> bindings = {{
        {0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eCompute},        // Uniforms
        {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},        // Objects
        {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},        // Draw commands
        {3, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},        // Visibility
        {4, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute}         // Stats
    }};

    computeDescriptorLayout = context->GetDevice().createDescriptorSetLayoutUnique(
        vk::DescriptorSetLayoutCreateInfo({}, bindings));

    // Create pipeline layout
    computePipelineLayout = context->GetDevice().createPipelineLayoutUnique(
        vk::PipelineLayoutCreateInfo({}, *computeDescriptorLayout));

    // Create compute pipelines
    vk::ComputePipelineCreateInfo cullingPipelineInfo(
        {}, vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eCompute, *cullingShader, "main"),
        *computePipelineLayout);

    cullingPipeline = context->GetDevice().createComputePipelineUnique(nullptr, cullingPipelineInfo).value;

    vk::ComputePipelineCreateInfo tilePipelineInfo(
        {}, vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eCompute, *tileBinningShader, "main"),
        *computePipelineLayout);

    tileBinningPipeline = context->GetDevice().createComputePipelineUnique(nullptr, tilePipelineInfo).value;
}

void GPUDrivenRenderer::CreateBuffers()
{
    // Object buffer
    objectBuffer = std::make_unique<BufferData>(maxObjects * sizeof(ObjectData),
                  vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
                  vk::MemoryPropertyFlagBits::eDeviceLocal);

    // Indirect draw commands buffer
    indirectBuffer = std::make_unique<BufferData>(maxObjects * sizeof(DrawCommand),
                    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer,
                    vk::MemoryPropertyFlagBits::eDeviceLocal);

    // Visibility buffer
    visibilityBuffer = std::make_unique<BufferData>(maxObjects * sizeof(uint32_t),
                      vk::BufferUsageFlagBits::eStorageBuffer,
                      vk::MemoryPropertyFlagBits::eDeviceLocal);

    // Uniform buffer
    uniformBuffer = std::make_unique<BufferData>(sizeof(CullingUniforms),
                   vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eTransferDst,
                   vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

    // Stats buffer
    statsBuffer = std::make_unique<BufferData>(sizeof(GPUStats),
                 vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc,
                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

    // Tile buffer for mobile optimization
    tileBuffer = std::make_unique<BufferData>(maxTiles * sizeof(TileData),
                vk::BufferUsageFlagBits::eStorageBuffer,
                vk::MemoryPropertyFlagBits::eDeviceLocal);
}

void GPUDrivenRenderer::CreateDescriptorSets()
{
    // Create descriptor pool
    std::array<vk::DescriptorPoolSize, 2> poolSizes = {{
        {vk::DescriptorType::eUniformBuffer, 1},
        {vk::DescriptorType::eStorageBuffer, 8}
    }};

    computeDescriptorPool = context->GetDevice().createDescriptorPoolUnique(
        vk::DescriptorPoolCreateInfo({}, 1, poolSizes));

    // Allocate descriptor set
    vk::DescriptorSetAllocateInfo allocInfo(*computeDescriptorPool, *computeDescriptorLayout);
    computeDescriptorSet = context->GetDevice().allocateDescriptorSets(allocInfo)[0];

    // Update descriptor set
    std::vector<vk::DescriptorBufferInfo> bufferInfos = {
        {*uniformBuffer->buffer, 0, sizeof(CullingUniforms)},
        {*objectBuffer->buffer, 0, VK_WHOLE_SIZE},
        {*indirectBuffer->buffer, 0, VK_WHOLE_SIZE},
        {*visibilityBuffer->buffer, 0, VK_WHOLE_SIZE},
        {*statsBuffer->buffer, 0, VK_WHOLE_SIZE}
    };

    std::vector<vk::WriteDescriptorSet> descriptorWrites = {
        {computeDescriptorSet, 0, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &bufferInfos[0]},
        {computeDescriptorSet, 1, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &bufferInfos[1]},
        {computeDescriptorSet, 2, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &bufferInfos[2]},
        {computeDescriptorSet, 3, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &bufferInfos[3]},
        {computeDescriptorSet, 4, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &bufferInfos[4]}
    };

    context->GetDevice().updateDescriptorSets(descriptorWrites, {});
}

vk::UniqueShaderModule GPUDrivenRenderer::CreateShaderModule(const std::vector<uint32_t>& spirv)
{
    return context->GetDevice().createShaderModuleUnique(
        vk::ShaderModuleCreateInfo({}, spirv.size() * sizeof(uint32_t), spirv.data()));
}

std::vector<uint32_t> GPUDrivenRenderer::CompileCullingShader()
{
    // For now, return empty vector - would need proper GLSL to SPIR-V compilation
    // In a real implementation, you'd use shaderc or glslang here
    WARN_LOG(RENDERER, "GPU-Driven culling shader compilation not implemented - using fallback");
    return {};
}

std::vector<uint32_t> GPUDrivenRenderer::CompileTileBinningShader()
{
    // For now, return empty vector - would need proper GLSL to SPIR-V compilation
    WARN_LOG(RENDERER, "GPU-Driven tile binning shader compilation not implemented - using fallback");
    return {};
}

void GPUDrivenRenderer::UploadObjects(const std::vector<ObjectData>& objects)
{
    if (!IsEnabled() || objects.empty()) {
        return;
    }

    // Upload object data to GPU
    // This would use a staging buffer in a real implementation
    stats.totalObjects = static_cast<uint32_t>(objects.size());
}

void GPUDrivenRenderer::PerformCulling(const CullingUniforms& uniforms)
{
    if (!IsEnabled()) {
        return;
    }

    // Reset stats
    stats.visibleObjects = 0;
    stats.culledObjects = 0;
    stats.drawCalls = 0;

    // Would dispatch compute shader here
    WARN_LOG(RENDERER, "GPU culling dispatch not implemented - using CPU fallback");
}

void GPUDrivenRenderer::ExecuteIndirectDraws(vk::CommandBuffer cmdBuffer, vk::Pipeline pipeline)
{
    if (!IsEnabled() || stats.visibleObjects == 0) {
        return;
    }

    // Would execute vkCmdDrawIndirect here
    WARN_LOG(RENDERER, "Indirect drawing not implemented - using regular draws");
}
