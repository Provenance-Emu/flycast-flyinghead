/*
 *  Created on: Oct 3, 2019

	Copyright 2019 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "texture.h"
#include "log/LogManager.h"

#include <algorithm>
#include <memory>

#ifdef __APPLE__
#include <sys/sysctl.h>
#if TARGET_OS_IOS || TARGET_OS_TV
#include "texture_streaming_ios.h"
#include "hw/pvr/ta_neon_optimizations.h"
#endif
#endif

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>

/// NEON-optimized texture upload for ARM devices with iOS enhancements
void optimized_texture_upload(void* dst, const void* src, int width, int height, int bytes_per_pixel, int src_stride, int dst_stride)
{
    const uint8_t* s = (const uint8_t*)src;
    uint8_t* d = (uint8_t*)dst;

    const int row_bytes = width * bytes_per_pixel;
    const size_t totalSize = height * row_bytes;

#ifdef __APPLE__
#if TARGET_OS_IOS || TARGET_OS_TV
    // Use iOS Texture Streaming Manager for larger textures
    if (totalSize >= 64 * 1024) { // 64KB threshold
        try {
            auto& streamingMgr = flycast::IOSTextureStreamingManager::Instance();

            // Use streaming manager's optimized copy for large textures
            // Use NEON-optimized memory operations for faster copying
            flycast::NEONMemOps::MemcpyNEON(dst, src, totalSize);
            DEBUG_LOG(RENDERER, "🚀 iOS NEON: Fast copy for %dx%d texture (%zu KB)",
                     width, height, totalSize / 1024);
            return;
        } catch (const std::exception& e) {
            DEBUG_LOG(RENDERER, "⚠️ iOS NEON copy failed, using fallback: %s", e.what());
        }
    }
#endif
#endif

    // Standard NEON implementation for non-iOS or smaller textures
    // If strides match and we can do a single bulk copy
    if (src_stride == dst_stride && src_stride == row_bytes) {
        // Use NEON for bulk copy when size is large enough and properly aligned
        if (totalSize >= 64 && (totalSize % 32) == 0) {
            const size_t vecSize = totalSize & ~31; // Process 32 bytes at a time

            for (size_t i = 0; i < vecSize; i += 32) {
                uint8x16_t v1 = vld1q_u8(s + i);
                uint8x16_t v2 = vld1q_u8(s + i + 16);
                vst1q_u8(d + i, v1);
                vst1q_u8(d + i + 16, v2);
            }

            // Copy remaining bytes with regular memcpy
            if (vecSize < totalSize) {
                memcpy(d + vecSize, s + vecSize, totalSize - vecSize);
            }
        } else {
            memcpy(d, s, totalSize);
        }
    } else {
        // Row-by-row copy when strides differ
        for (int y = 0; y < height; y++) {
            // Use NEON for row copy if row is large enough
            if (row_bytes >= 64 && (row_bytes % 32) == 0) {
                const uint8_t* src_row = s + y * src_stride;
                uint8_t* dst_row = d + y * dst_stride;

                for (int i = 0; i < row_bytes; i += 32) {
                    uint8x16_t v1 = vld1q_u8(src_row + i);
                    uint8x16_t v2 = vld1q_u8(src_row + i + 16);
                    vst1q_u8(dst_row + i, v1);
                    vst1q_u8(dst_row + i + 16, v2);
                }
            } else {
                memcpy(d + y * dst_stride, s + y * src_stride, row_bytes);
            }
        }
    }
}
#endif

/// iOS device memory tier detection for texture streaming optimization
#ifdef __APPLE__
#if TARGET_OS_IOS
enum class IOSDeviceMemoryTier {
    LOW_MEMORY,    // <2GB total memory (older iPads)
    MEDIUM_MEMORY, // 2-4GB total memory
    HIGH_MEMORY    // >4GB total memory (modern devices)
};

static IOSDeviceMemoryTier detectIOSDeviceMemoryTier() {
    static IOSDeviceMemoryTier cachedTier = IOSDeviceMemoryTier::MEDIUM_MEMORY;
    static bool detected = false;

    if (!detected) {
        size_t total_memory = 0;
        size_t length = sizeof(total_memory);

        if (sysctlbyname("hw.memsize", &total_memory, &length, nullptr, 0) == 0) {
            const size_t GB = 1024ULL * 1024ULL * 1024ULL;
            if (total_memory < 2 * GB) {
                cachedTier = IOSDeviceMemoryTier::LOW_MEMORY;
                INFO_LOG(RENDERER, "🔧 iOS Low Memory Device Detected: %.2f GB",
                         total_memory / (1024.0 * 1024.0 * 1024.0));
            } else if (total_memory < 4 * GB) {
                cachedTier = IOSDeviceMemoryTier::MEDIUM_MEMORY;
                INFO_LOG(RENDERER, "📱 iOS Medium Memory Device: %.2f GB",
                         total_memory / (1024.0 * 1024.0 * 1024.0));
            } else {
                cachedTier = IOSDeviceMemoryTier::HIGH_MEMORY;
                INFO_LOG(RENDERER, "🚀 iOS High Memory Device: %.2f GB",
                         total_memory / (1024.0 * 1024.0 * 1024.0));
            }
        }
        detected = true;
    }
    return cachedTier;
}

/// Get optimal texture upload strategy based on device memory
static bool shouldUseConservativeTextureStreaming() {
    return detectIOSDeviceMemoryTier() == IOSDeviceMemoryTier::LOW_MEMORY;
}
#else
static bool shouldUseConservativeTextureStreaming() { return false; }
#endif
#else
static bool shouldUseConservativeTextureStreaming() { return false; }
#endif

void setImageLayout(vk::CommandBuffer const& commandBuffer, vk::Image image, vk::Format format, u32 mipmapLevels, vk::ImageLayout oldImageLayout, vk::ImageLayout newImageLayout)
{
	static const float scopeColor[4] = { 0.75f, 0.75f, 0.0f, 1.0f };
	CommandBufferDebugScope _(commandBuffer, "setImageLayout", scopeColor);

	vk::AccessFlags sourceAccessMask;
	switch (oldImageLayout)
	{
	case vk::ImageLayout::eTransferDstOptimal:
		sourceAccessMask = vk::AccessFlagBits::eTransferWrite;
		break;
	case vk::ImageLayout::eTransferSrcOptimal:
		sourceAccessMask = vk::AccessFlagBits::eTransferRead;
		break;
	case vk::ImageLayout::ePreinitialized:
		sourceAccessMask = vk::AccessFlagBits::eHostWrite;
		break;
	case vk::ImageLayout::eGeneral:     // sourceAccessMask is empty
	case vk::ImageLayout::eUndefined:
		break;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		sourceAccessMask = vk::AccessFlagBits::eShaderRead;
		break;
	default:
		verify(false);
		break;
	}

	vk::PipelineStageFlags sourceStage;
	switch (oldImageLayout)
	{
	case vk::ImageLayout::eGeneral:
	case vk::ImageLayout::ePreinitialized:
		sourceStage = vk::PipelineStageFlagBits::eHost;
		break;
	case vk::ImageLayout::eTransferDstOptimal:
	case vk::ImageLayout::eTransferSrcOptimal:
		sourceStage = vk::PipelineStageFlagBits::eTransfer;
		break;
	case vk::ImageLayout::eUndefined:
		sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
		break;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		sourceStage = vk::PipelineStageFlagBits::eFragmentShader;
		break;
	default:
		verify(false);
		break;
	}

	vk::AccessFlags destinationAccessMask;
	switch (newImageLayout)
	{
	case vk::ImageLayout::eColorAttachmentOptimal:
		destinationAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
		break;
	case vk::ImageLayout::eDepthStencilAttachmentOptimal:
		destinationAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
		break;
	case vk::ImageLayout::eGeneral:   // empty destinationAccessMask
		break;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		destinationAccessMask = vk::AccessFlagBits::eShaderRead;
		break;
	case vk::ImageLayout::eTransferSrcOptimal:
		destinationAccessMask = vk::AccessFlagBits::eTransferRead;
		break;
	case vk::ImageLayout::eTransferDstOptimal:
		destinationAccessMask = vk::AccessFlagBits::eTransferWrite;
		break;
	case vk::ImageLayout::eDepthStencilReadOnlyOptimal:
		destinationAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentRead;
		break;
	default:
		verify(false);
		break;
	}

	vk::PipelineStageFlags destinationStage;
	switch (newImageLayout)
	{
	case vk::ImageLayout::eColorAttachmentOptimal:
		destinationStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
		break;
	case vk::ImageLayout::eDepthStencilAttachmentOptimal:
		destinationStage = vk::PipelineStageFlagBits::eEarlyFragmentTests;
		break;
	case vk::ImageLayout::eGeneral:
		destinationStage = vk::PipelineStageFlagBits::eHost;
		break;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		destinationStage = vk::PipelineStageFlagBits::eFragmentShader;
		break;
	case vk::ImageLayout::eTransferDstOptimal:
	case vk::ImageLayout::eTransferSrcOptimal:
		destinationStage = vk::PipelineStageFlagBits::eTransfer;
		break;
	case vk::ImageLayout::eDepthStencilReadOnlyOptimal:
		destinationStage = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
		break;
	default:
		verify(false);
		break;
	}

	vk::ImageAspectFlags aspectMask;
	if (newImageLayout == vk::ImageLayout::eDepthStencilAttachmentOptimal || newImageLayout == vk::ImageLayout::eDepthStencilReadOnlyOptimal)
	{
		aspectMask = vk::ImageAspectFlagBits::eDepth;
		if (format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint || format == vk::Format::eD16UnormS8Uint)
		{
			aspectMask |= vk::ImageAspectFlagBits::eStencil;
		}
	}
	else
	{
		aspectMask = vk::ImageAspectFlagBits::eColor;
	}

	vk::ImageSubresourceRange imageSubresourceRange(aspectMask, 0, mipmapLevels, 0, 1);
	vk::ImageMemoryBarrier imageMemoryBarrier(sourceAccessMask, destinationAccessMask, oldImageLayout, newImageLayout, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, imageSubresourceRange);
	commandBuffer.pipelineBarrier(sourceStage, destinationStage, {}, nullptr, nullptr, imageMemoryBarrier);
}

void Texture::UploadToGPU(int width, int height, const u8 *data, bool mipmapped, bool mipmapsIncluded)
{
	vk::Format format = vk::Format::eUndefined;
	u32 dataSize = width * height * 2;
	switch (tex_type)
	{
	case TextureType::_5551:
		format = vk::Format::eR5G5B5A1UnormPack16;
		break;
	case TextureType::_565:
		format = vk::Format::eR5G6B5UnormPack16;
		break;
	case TextureType::_4444:
		format = vk::Format::eR4G4B4A4UnormPack16;
		break;
	case TextureType::_8888:
		format = vk::Format::eR8G8B8A8Unorm;
		dataSize *= 2;
		break;
	case TextureType::_8:
		format = vk::Format::eR8Unorm;
		dataSize /= 2;
		break;
	}
	if (mipmapsIncluded)
	{
		int w = width / 2;
		u32 size = dataSize / 4;
		while (w)
		{
			dataSize += ((size + 3) >> 2) << 2;		// offset must be a multiple of 4
			size /= 4;
			w /= 2;
		}
	}
	bool isNew = true;
	if (width != (int)extent.width || height != (int)extent.height
			|| format != this->format || !this->image)
		Init(width, height, format, dataSize, mipmapped, mipmapsIncluded);
	else
		isNew = false;
	SetImage(dataSize, data, isNew, mipmapped && !mipmapsIncluded);
}

void Texture::Init(u32 width, u32 height, vk::Format format, u32 dataSize, bool mipmapped, bool mipmapsIncluded)
{
	this->extent = vk::Extent2D(width, height);
	this->format = format;
	mipmapLevels = 1;
	if (mipmapped)
		mipmapLevels += floor(log2(std::max(width, height)));

	vk::FormatProperties formatProperties = physicalDevice.getFormatProperties(format);

	vk::ImageTiling imageTiling = (formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage)
			== vk::FormatFeatureFlagBits::eSampledImage
			? vk::ImageTiling::eOptimal
			: vk::ImageTiling::eLinear;
#ifndef __APPLE__
	// Texture corruption with moltenvk. Perf improvement on other platforms
	if (height <= 32
			&& dataSize / height <= 64
			&& !mipmapped
			&& (formatProperties.linearTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage) == vk::FormatFeatureFlagBits::eSampledImage)
		imageTiling = vk::ImageTiling::eLinear;
#endif
	needsStaging = imageTiling != vk::ImageTiling::eLinear;
	vk::ImageLayout initialLayout;
	vk::ImageUsageFlags usageFlags = vk::ImageUsageFlagBits::eSampled;
	if (needsStaging)
	{
		stagingBufferData = std::make_unique<BufferData>(dataSize, vk::BufferUsageFlagBits::eTransferSrc);
		usageFlags |= vk::ImageUsageFlagBits::eTransferDst;
		initialLayout = vk::ImageLayout::eUndefined;
	}
	else
	{
		verify((formatProperties.linearTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage) == vk::FormatFeatureFlagBits::eSampledImage);
		initialLayout = vk::ImageLayout::ePreinitialized;
	}
	if (mipmapped && !mipmapsIncluded)
		usageFlags |= vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
	CreateImage(imageTiling, usageFlags, initialLayout, vk::ImageAspectFlagBits::eColor);
}

void Texture::CreateImage(vk::ImageTiling tiling, vk::ImageUsageFlags usage, vk::ImageLayout initialLayout,
		vk::ImageAspectFlags aspectMask)
{
	this->usageFlags = usage;
	vk::ImageCreateInfo imageCreateInfo(vk::ImageCreateFlags(), vk::ImageType::e2D, format, vk::Extent3D(extent, 1), mipmapLevels, 1,
										vk::SampleCountFlagBits::e1, tiling, usage,
										vk::SharingMode::eExclusive, nullptr, initialLayout);
	image = device.createImageUnique(imageCreateInfo);

	VmaAllocationCreateInfo allocCreateInfo = { VmaAllocationCreateFlags(), needsStaging ? VmaMemoryUsage::VMA_MEMORY_USAGE_GPU_ONLY : VmaMemoryUsage::VMA_MEMORY_USAGE_CPU_TO_GPU };
#ifndef __APPLE__
	if (!needsStaging)
		allocCreateInfo.flags = VmaAllocationCreateFlagBits::VMA_ALLOCATION_CREATE_MAPPED_BIT;
#else
	/// iOS memory optimization: use streaming manager for device-specific allocation
#if TARGET_OS_IOS || TARGET_OS_TV
	try {
		auto& streamingMgr = flycast::IOSTextureStreamingManager::Instance();
		auto deviceTier = streamingMgr.GetDeviceTier();

		if (deviceTier == flycast::IOSTextureStreamingManager::DevicePerformanceTier::LOW_PERFORMANCE) {
			// Conservative allocation for older devices
			DEBUG_LOG(RENDERER, "🔧 iOS Low Memory: Using conservative texture allocation (%dx%d)", extent.width, extent.height);
			if (!needsStaging) {
				allocCreateInfo.flags = VmaAllocationCreateFlagBits::VMA_ALLOCATION_CREATE_MAPPED_BIT;
			}
		} else if (deviceTier == flycast::IOSTextureStreamingManager::DevicePerformanceTier::HIGH_PERFORMANCE) {
			// Aggressive allocation for high-performance devices
			DEBUG_LOG(RENDERER, "🚀 iOS High Performance: Using dedicated texture allocation (%dx%d)", extent.width, extent.height);
			if (!needsStaging) {
				allocCreateInfo.flags = VmaAllocationCreateFlagBits::VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
			}
		} else {
			// Medium performance - balanced approach
			DEBUG_LOG(RENDERER, "📱 iOS Medium Performance: Using balanced texture allocation (%dx%d)", extent.width, extent.height);
			if (!needsStaging) {
				allocCreateInfo.flags = VmaAllocationCreateFlagBits::VMA_ALLOCATION_CREATE_MAPPED_BIT;
			}
		}
	} catch (const std::exception& e) {
		// Fallback to original iOS logic
		DEBUG_LOG(RENDERER, "iOS streaming manager not available, using fallback: %s", e.what());
		if (shouldUseConservativeTextureStreaming()) {
			DEBUG_LOG(RENDERER, "🔧 iOS Low Memory: Using conservative texture allocation (%dx%d)", extent.width, extent.height);
		} else if (!needsStaging) {
			allocCreateInfo.flags = VmaAllocationCreateFlagBits::VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
		}
	}
#else
	/// Original iOS memory optimization for non-iOS/tvOS Apple platforms
	if (shouldUseConservativeTextureStreaming()) {
		// On low-memory devices, avoid dedicated allocation to conserve memory
		// This helps older iPads with limited RAM avoid memory pressure
		DEBUG_LOG(RENDERER, "🔧 iOS Low Memory: Using conservative texture allocation (%dx%d)", extent.width, extent.height);
	} else if (!needsStaging) {
		// On higher-memory devices, use dedicated allocation for better performance
		allocCreateInfo.flags = VmaAllocationCreateFlagBits::VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
	}
#endif
#endif
	allocation = VulkanContext::Instance()->GetAllocator().AllocateForImage(*image, allocCreateInfo);

	vk::ImageViewCreateInfo imageViewCreateInfo(vk::ImageViewCreateFlags(), image.get(), vk::ImageViewType::e2D, format, vk::ComponentMapping(),
			vk::ImageSubresourceRange(aspectMask, 0, mipmapLevels, 0, 1));
	imageView = device.createImageViewUnique(imageViewCreateInfo);
#ifdef VK_DEBUG
	char name[128];
	snprintf(name, sizeof(name), "texture @ %x", startAddress);
	VulkanContext::Instance()->setObjectName(image.get(), name);
	VulkanContext::Instance()->setObjectName(imageView.get(), name);
#endif
}

void Texture::SetImage(u32 srcSize, const void *srcData, bool isNew, bool genMipmaps)
{
	verify((bool)commandBuffer);

	static const float scopeColor[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
	CommandBufferDebugScope _(commandBuffer, "SetImage", scopeColor);

	if (!isNew && !needsStaging)
		setImageLayout(commandBuffer, image.get(), format, mipmapLevels, vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eGeneral);

	void* data;
	if (needsStaging)
	{
		if (!stagingBufferData)
			// This can happen if a texture is first created for RTT, then later updated
			stagingBufferData = std::make_unique<BufferData>(srcSize, vk::BufferUsageFlagBits::eTransferSrc);
		data = stagingBufferData->MapMemory();
	}
	else
		data = allocation.MapMemory();
	verify(data != nullptr);

	/// iOS Texture Optimization: Apply device-specific optimizations for texture data
#ifdef __APPLE__
#if TARGET_OS_IOS || TARGET_OS_TV
	try {
		auto& streamingMgr = flycast::IOSTextureStreamingManager::Instance();

		// Check if we should apply iOS optimizations for larger textures
		if (srcSize >= 32 * 1024 && extent.width >= 64 && extent.height >= 64) {
			// Create TextureData structure for streaming manager
			flycast::IOSTextureStreamingManager::TextureData textureData;
			textureData.data = const_cast<void*>(srcData);
			textureData.width = extent.width;
			textureData.height = extent.height;

			// Determine format based on texture type
			switch (tex_type) {
				case TextureType::_8888:
					textureData.format = flycast::IOSTextureStreamingManager::TextureFormat::RGBA8;
					break;
				case TextureType::_565:
				case TextureType::_4444:
				case TextureType::_5551:
					textureData.format = flycast::IOSTextureStreamingManager::TextureFormat::RGBA8;
					break;
				case TextureType::_8:
					textureData.format = flycast::IOSTextureStreamingManager::TextureFormat::RGBA8;
					break;
				default:
					textureData.format = flycast::IOSTextureStreamingManager::TextureFormat::RGBA8;
					break;
			}

			// Apply device-specific optimizations
			streamingMgr.OptimizeTextureForDevice(textureData);

			DEBUG_LOG(RENDERER, "iOS: Applied texture optimization for %dx%d texture (size: %u KB)",
					  extent.width, extent.height, srcSize / 1024);
		}
		// Use streaming manager's optimized copy for large textures
		if (srcSize >= 32 * 1024) { // 32KB threshold for TA optimizations
			try {
				// Use NEON-optimized TA data copy for large texture operations
				flycast::NEONTAProcessor::CopyTADataNEON(data, srcData, srcSize);
				DEBUG_LOG(RENDERER, "🔧 iOS NEON TA: Optimized copy for %u KB texture data", srcSize / 1024);
			} catch (const std::exception& e) {
				DEBUG_LOG(RENDERER, "⚠️ iOS NEON TA copy failed, using standard copy: %s", e.what());
				// Fall back to standard copy
				memcpy(data, srcData, srcSize);
			}
		} else {
			// Use standard copy for smaller textures
			memcpy(data, srcData, srcSize);
		}
	} catch (const std::exception& e) {
		DEBUG_LOG(RENDERER, "iOS texture optimization failed: %s", e.what());
	}
#endif
#endif

	if (mipmapLevels > 1 && !genMipmaps && tex_type != TextureType::_8888)
	{
		// Each mipmap level must start at a 4-byte boundary
		u8 *src = (u8 *)srcData;
		u8 *dst = (u8 *)data;
		for (u32 i = 0; i < mipmapLevels; i++)
		{
			const u32 size = (1 << (2 * i)) * 2;

			/// Use NEON-optimized copy for ARM devices when available
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
			if (size >= 32) {
				// Use optimized copy for larger transfers
				const size_t vecSize = size & ~31;
				for (size_t j = 0; j < vecSize; j += 32) {
					uint8x16_t v1 = vld1q_u8(src + j);
					uint8x16_t v2 = vld1q_u8(src + j + 16);
					vst1q_u8(dst + j, v1);
					vst1q_u8(dst + j + 16, v2);
				}
				// Copy remaining bytes
				for (size_t j = vecSize; j < size; j++)
					dst[j] = src[j];
			} else {
				memcpy(dst, src, size);
			}
#else
			memcpy(dst, src, size);
#endif

			dst += ((size + 3) >> 2) << 2;
			src += size;
		}
	}
	else if (!needsStaging)
	{
		vk::SubresourceLayout layout = device.getImageSubresourceLayout(*image, vk::ImageSubresource(vk::ImageAspectFlagBits::eColor));
		if (layout.size != srcSize)
		{
			u8 *src = (u8 *)srcData;
			u8 *dst = (u8 *)data;
			u32 srcSz = extent.width * 2;
			if (tex_type == TextureType::_8888)
				srcSz *= 2;
			else if (tex_type == TextureType::_8)
				srcSz /= 2;
			u8 * const srcEnd = src + srcSz * extent.height;

			/// Use NEON-optimized texture upload for iOS devices
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
			// Calculate bytes per pixel correctly
			int bytes_per_pixel = 2; // Default for most formats
			if (tex_type == TextureType::_8888)
				bytes_per_pixel = 4;
			else if (tex_type == TextureType::_8)
				bytes_per_pixel = 1;

			// For iOS devices, use optimized upload when beneficial and safe
			const bool useOptimizedUpload = (srcSz >= 64) && (extent.height > 1) && (srcSz == extent.width * bytes_per_pixel);
			if (useOptimizedUpload) {
				optimized_texture_upload(dst, src, extent.width, extent.height,
					bytes_per_pixel, srcSz, (int)layout.rowPitch);
			} else {
				for (; src < srcEnd; src += srcSz, dst += layout.rowPitch)
					memcpy(dst, src, srcSz);
			}
#else
			for (; src < srcEnd; src += srcSz, dst += layout.rowPitch)
				memcpy(dst, src, srcSz);
#endif
		}
		else {
			/// Use NEON-optimized copy for larger textures on ARM devices
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
			// Calculate bytes per pixel correctly
			int bytes_per_pixel = 2; // Default for most formats
			if (tex_type == TextureType::_8888)
				bytes_per_pixel = 4;
			else if (tex_type == TextureType::_8)
				bytes_per_pixel = 1;

			int stride = extent.width * bytes_per_pixel;
			if (srcSize >= 64 && srcSize == extent.width * extent.height * bytes_per_pixel) {
				optimized_texture_upload(data, srcData, extent.width, extent.height,
					bytes_per_pixel, stride, stride);
			} else {
				memcpy(data, srcData, srcSize);
			}
#else
			memcpy(data, srcData, srcSize);
#endif
		}
		allocation.UnmapMemory();
	}
	else {
		/// Use NEON-optimized copy for staging buffers on ARM devices
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
		// Calculate bytes per pixel correctly
		int bytes_per_pixel = 2; // Default for most formats
		if (tex_type == TextureType::_8888)
			bytes_per_pixel = 4;
		else if (tex_type == TextureType::_8)
			bytes_per_pixel = 1;

		int stride = extent.width * bytes_per_pixel;
		if (srcSize >= 64 && srcSize == extent.width * extent.height * bytes_per_pixel) {
			optimized_texture_upload(data, srcData, extent.width, extent.height,
				bytes_per_pixel, stride, stride);
		} else {
			memcpy(data, srcData, srcSize);
		}
#else
		memcpy(data, srcData, srcSize);
#endif
	}

	if (needsStaging)
	{
		stagingBufferData->UnmapMemory();
		// Since we're going to blit to the texture image, set its layout to eTransferDstOptimal
		setImageLayout(commandBuffer, image.get(), format, mipmapLevels, isNew ? vk::ImageLayout::eUndefined : vk::ImageLayout::eShaderReadOnlyOptimal,
				vk::ImageLayout::eTransferDstOptimal);

		if (mipmapLevels > 1 && !genMipmaps)
		{
			vk::DeviceSize bufferOffset = 0;
			for (u32 i = 0; i < mipmapLevels; i++)
			{
				vk::BufferImageCopy copyRegion(bufferOffset, 1 << i, 1 << i, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, mipmapLevels - i - 1, 0, 1),
						vk::Offset3D(0, 0, 0), vk::Extent3D(1 << i, 1 << i, 1));
				commandBuffer.copyBufferToImage(stagingBufferData->buffer.get(), image.get(), vk::ImageLayout::eTransferDstOptimal, copyRegion);
				const u32 size = (1 << (2 * i)) * (tex_type == TextureType::_8888 ? 4 : 2);
				bufferOffset += ((size + 3) >> 2) << 2;
			}
		}
		else
		{
			vk::BufferImageCopy copyRegion(0, extent.width, extent.height, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
					vk::Offset3D(0, 0, 0), vk::Extent3D(extent, 1));
			commandBuffer.copyBufferToImage(stagingBufferData->buffer.get(), image.get(), vk::ImageLayout::eTransferDstOptimal, copyRegion);
			if (mipmapLevels > 1)
				GenerateMipmaps();
		}
		// Set the layout for the texture image from eTransferDstOptimal to SHADER_READ_ONLY
		setImageLayout(commandBuffer, image.get(), format, mipmapLevels, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
	}
	else
	{
		if (mipmapLevels > 1)
			GenerateMipmaps();
		else
			// If we can use the linear tiled image as a texture, just do it
			setImageLayout(commandBuffer, image.get(), format, mipmapLevels, isNew ? vk::ImageLayout::ePreinitialized : vk::ImageLayout::eGeneral,
					vk::ImageLayout::eShaderReadOnlyOptimal);
	}
}

void Texture::GenerateMipmaps()
{
	static const float scopeColor[4] = { 0.75f, 0.75f, 0.0f, 1.0f };
	CommandBufferDebugScope _(commandBuffer, "GenerateMipmaps", scopeColor);

	u32 mipWidth = extent.width;
	u32 mipHeight = extent.height;
	vk::ImageMemoryBarrier barrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead,
			vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
			*image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));

	for (u32 i = 1; i < mipmapLevels; i++)
	{
		// Transition previous mipmap level from dst optimal/preinit to src optimal
		barrier.subresourceRange.baseMipLevel = i - 1;
		if (i == 1 && !needsStaging)
		{
			barrier.oldLayout = vk::ImageLayout::ePreinitialized;
			barrier.srcAccessMask = vk::AccessFlagBits::eHostWrite;
		}
		else
		{
			barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
			barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		}
		barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
		barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
		commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, barrier);

		// Blit previous mipmap level on current
		vk::ImageBlit blit(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, i - 1, 0, 1),
				 { { vk::Offset3D(0, 0, 0), vk::Offset3D(mipWidth, mipHeight, 1) } },
				 vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, i, 0, 1),
				 { { vk::Offset3D(0, 0, 0), vk::Offset3D(std::max(mipWidth / 2, 1u), std::max(mipHeight / 2, 1u), 1) } });
		commandBuffer.blitImage(*image, vk::ImageLayout::eTransferSrcOptimal, *image, vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);

		// Transition previous mipmap level from src optimal to shader read-only optimal
		barrier.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
		barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
		barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
		barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
		commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, {}, nullptr, nullptr, barrier);

		mipWidth = std::max(mipWidth / 2, 1u);
		mipHeight = std::max(mipHeight / 2, 1u);
	}
	// Transition last mipmap level from dst optimal to shader read-only optimal
	barrier.subresourceRange.baseMipLevel = mipmapLevels - 1;
	barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
	barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
	commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, {}, nullptr, nullptr, barrier);
}

void Texture::deferDeleteResource(FlightManager *manager)
{
	class ResourceDeleter : public Deletable
	{
	public:
		ResourceDeleter(Texture *texture)
		{
			std::swap(image, texture->image);
			std::swap(imageView, texture->imageView);
			std::swap(bufferData, texture->stagingBufferData);
			std::swap(allocation, texture->allocation);
		}

	private:
		vk::UniqueImage image;
		vk::UniqueImageView imageView;
		std::unique_ptr<BufferData> bufferData;
		Allocation allocation;
	};
	manager->addToFlight(new ResourceDeleter(this));
}

void FramebufferAttachment::Init(u32 width, u32 height, vk::Format format, const vk::ImageUsageFlags& usage, const std::string& name)
{
	this->format = format;
	this->extent = vk::Extent2D { width, height };
	bool depth = format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint || format == vk::Format::eD16UnormS8Uint;

	if (usage & vk::ImageUsageFlagBits::eTransferSrc)
	{
		stagingBufferData = std::make_unique<BufferData>(width * height * 4,
				vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
				vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached | vk::MemoryPropertyFlagBits::eHostCoherent);
	}
	vk::ImageCreateInfo imageCreateInfo(vk::ImageCreateFlags(), vk::ImageType::e2D, format, vk::Extent3D(extent, 1), 1, 1, vk::SampleCountFlagBits::e1,
			vk::ImageTiling::eOptimal, usage,
			vk::SharingMode::eExclusive, nullptr, vk::ImageLayout::eUndefined);
	image = device.createImageUnique(imageCreateInfo);
#ifdef VK_DEBUG
	if (!name.empty())
		VulkanContext::Instance()->setObjectName(image.get(), name);
#endif

	VmaAllocationCreateInfo allocCreateInfo = { VmaAllocationCreateFlags(), VmaMemoryUsage::VMA_MEMORY_USAGE_GPU_ONLY };
	if (usage & vk::ImageUsageFlagBits::eTransientAttachment)
		allocCreateInfo.preferredFlags = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
	allocation = VulkanContext::Instance()->GetAllocator().AllocateForImage(*image, allocCreateInfo);

	if ((usage & vk::ImageUsageFlagBits::eColorAttachment) || (usage & vk::ImageUsageFlagBits::eDepthStencilAttachment))
	{
		vk::ImageViewCreateInfo imageViewCreateInfo(vk::ImageViewCreateFlags(), image.get(), vk::ImageViewType::e2D,
				format, vk::ComponentMapping(),	vk::ImageSubresourceRange(depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
		imageView = device.createImageViewUnique(imageViewCreateInfo);
#ifdef VK_DEBUG
		if (!name.empty())
			VulkanContext::Instance()->setObjectName(imageView.get(), name);
#endif

		if ((usage & vk::ImageUsageFlagBits::eDepthStencilAttachment) && (usage & vk::ImageUsageFlagBits::eInputAttachment))
		{
			// Also create an imageView for the stencil
			imageViewCreateInfo.subresourceRange = vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eStencil, 0, 1, 0, 1);
			stencilView = device.createImageViewUnique(imageViewCreateInfo);
#ifdef VK_DEBUG
			if (!name.empty())
				VulkanContext::Instance()->setObjectName(stencilView.get(), name);
#endif
		}
	}
}

void TextureCache::Cleanup()
{
	std::vector<u64> list;

	u32 TargetFrame = std::max((u32)120, FrameCount) - 120;

	for (const auto& [id, texture] : cache)
	{
		if (texture.dirty && texture.dirty < TargetFrame)
			list.push_back(id);

		if (list.size() > 5)
			break;
	}

	for (u64 id : list)
	{
		if (clearTexture(&cache[id]))
			cache.erase(id);
	}
}
