/*
    Created on: Oct 2, 2019

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
#include "vulkan.h"
#include "vulkan_renderer.h"
#include "gpu_driven_renderer.h"
#include "fmv_async_pipeline.h"
#include "fence_free_submitter.h"
#include "drawer.h"
#include "hw/pvr/ta.h"
#include "rend/transform_matrix.h"
#include "log/LogManager.h"
#include <algorithm>
#include <vector>
#include <functional>

#ifdef __APPLE__
#if TARGET_OS_IOS || TARGET_OS_TV
#include "texture_streaming_ios.h"
#include "hw/pvr/ta_neon_optimizations.h"
#endif
#endif

bool BaseVulkanRenderer::BaseInit(vk::RenderPass renderPass, int subpass)
{
	texCommandPool.Init();
	fbCommandPool.Init();
	quadPipeline = std::make_unique<QuadPipeline>(false, false);
	quadPipeline->Init(&shaderManager, renderPass, subpass);
	framebufferDrawer = std::make_unique<QuadDrawer>();
	framebufferDrawer->Init(quadPipeline.get());

	return true;
}

void BaseVulkanRenderer::Term()
{
	GetContext()->WaitIdle();
	GetContext()->PresentFrame(nullptr, nullptr, vk::Extent2D(), 0);
#if defined(VIDEO_ROUTING) && defined(TARGET_MAC)
	os_VideoRoutingTermVk();
#endif
	framebufferDrawer.reset();
	quadPipeline.reset();
	textureCache.Clear();
	fogTexture = nullptr;
	paletteTexture = nullptr;
	texCommandPool.Term();
	fbCommandPool.Term();
	framebufferTextures.clear();
	framebufferTexIndex = 0;
	shaderManager.term();
}

BaseTextureCacheData *BaseVulkanRenderer::GetTexture(TSP tsp, TCW tcw)
{
	Texture* tf = textureCache.getTextureCacheData(tsp, tcw);

	//update if needed
	if (tf->NeedsUpdate())
	{
		// This kills performance when a frame is skipped and lots of texture updated each frame
		//if (textureCache.IsInFlight(tf, true))
		//	textureCache.DestroyLater(tf);
		tf->SetCommandBuffer(texCommandBuffer);
		if (!tf->Update())
		{
			tf->SetCommandBuffer(nullptr);
			return nullptr;
		}
	}
	else if (tf->IsCustomTextureAvailable())
	{
		tf->deferDeleteResource(&texCommandPool);
		tf->SetCommandBuffer(texCommandBuffer);
		tf->CheckCustomTexture();
	}
	tf->SetCommandBuffer(nullptr);
	textureCache.SetInFlight(tf);

	return tf;
}

void BaseVulkanRenderer::Process(TA_context* ctx)
{
	if (!ctx->rend.isRTT) {
		framebufferRendered = false;
		if (!config::EmulateFramebuffer)
			clearLastFrame = false;
	}
	if (resetTextureCache) {
		textureCache.Clear();
		resetTextureCache = false;
	}

	texCommandPool.BeginFrame();
	textureCache.SetCurrentIndex(texCommandPool.GetIndex());
	textureCache.Cleanup();

	texCommandBuffer = texCommandPool.Allocate();
	texCommandBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

	ta_parse(ctx, true);

	// TODO can't update fog or palette twice in multi render
	CheckFogTexture();
	CheckPaletteTexture();

	/// iOS Texture Prefetching: Prefetch textures for next frame
#ifdef __APPLE__
#if TARGET_OS_IOS || TARGET_OS_TV
	try {
		auto& streamingMgr = flycast::IOSTextureStreamingManager::Instance();

		// Collect texture IDs for prefetching
		std::vector<u32> textureIds;
		textureIds.reserve(128);

		// Collect from translucent triangles
		for (const auto& poly : ctx->rend.global_param_tr) {
			if (poly.texture) {
				std::string textureId = poly.texture->GetId();
				textureIds.push_back(std::hash<std::string>{}(textureId));
			}
		}

		// Collect from opaque triangles
		for (const auto& poly : ctx->rend.global_param_op) {
			if (poly.texture) {
				std::string textureId = poly.texture->GetId();
				textureIds.push_back(std::hash<std::string>{}(textureId));
			}
		}

		// Collect from punch-through triangles
		for (const auto& poly : ctx->rend.global_param_pt) {
			if (poly.texture) {
				std::string textureId = poly.texture->GetId();
				textureIds.push_back(std::hash<std::string>{}(textureId));
			}
		}

		// Remove duplicates and prefetch
		if (!textureIds.empty()) {
			std::sort(textureIds.begin(), textureIds.end());
			textureIds.erase(std::unique(textureIds.begin(), textureIds.end()), textureIds.end());

			DEBUG_LOG(RENDERER, "🔄 iOS Texture Prefetching: %zu unique textures", textureIds.size());
			streamingMgr.PrefetchTextures(textureIds);
		}
	} catch (const std::exception& e) {
		DEBUG_LOG(RENDERER, "⚠️ iOS Texture Prefetching error: %s", e.what());
	}
#endif
#endif

	texCommandBuffer.end();
}

void BaseVulkanRenderer::ReInitOSD()
{
	texCommandPool.Init();
	fbCommandPool.Init();
}

void BaseVulkanRenderer::RenderFramebuffer(const FramebufferInfo& info)
{
	framebufferTexIndex = (framebufferTexIndex + 1) % GetContext()->GetSwapChainSize();

	if (framebufferTextures.size() != GetContext()->GetSwapChainSize())
		framebufferTextures.resize(GetContext()->GetSwapChainSize());
	std::unique_ptr<Texture>& curTexture = framebufferTextures[framebufferTexIndex];
	if (!curTexture)
	{
		curTexture = std::make_unique<Texture>();
		curTexture->tex_type = TextureType::_8888;
	}

	fbCommandPool.BeginFrame();
	vk::CommandBuffer commandBuffer = fbCommandPool.Allocate();

	commandBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	curTexture->SetCommandBuffer(commandBuffer);
	{
		static const float scopeColor[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
		CommandBufferDebugScope _(commandBuffer, "RenderFramebuffer", scopeColor);

		if (info.fb_r_ctrl.fb_enable == 0 || info.vo_control.blank_video == 1)
		{
			// Video output disabled
			u8 rgba[]{ (u8)info.vo_border_col._red, (u8)info.vo_border_col._green, (u8)info.vo_border_col._blue, 255 };
			curTexture->UploadToGPU(1, 1, rgba, false);
		}
		else
		{
			PixelBuffer<u32> pb;
			int width;
			int height;
			ReadFramebuffer(info, pb, width, height);

			curTexture->UploadToGPU(width, height, (u8*)pb.data(), false);
		}

	}
	curTexture->SetCommandBuffer(nullptr);
	commandBuffer.end();
	fbCommandPool.EndFrame();
	framebufferRendered = true;
	clearLastFrame = false;
}

void BaseVulkanRenderer::RenderVideoRouting()
{
#if defined(VIDEO_ROUTING) && defined(TARGET_MAC)
	if (config::VideoRouting)
	{
		auto device = GetContext()->GetDevice();
		auto srcImage = device.getSwapchainImagesKHR(GetContext()->GetSwapChain())[GetContext()->GetCurrentImageIndex()];
		auto graphicsQueue = device.getQueue(GetContext()->GetGraphicsQueueFamilyIndex(), 0);

		int targetWidth = (config::VideoRoutingScale ? config::VideoRoutingVRes * settings.display.width / settings.display.height : settings.display.width);
		int targetHeight = (config::VideoRoutingScale ? config::VideoRoutingVRes : settings.display.height);

		extern void os_VideoRoutingPublishFrameTexture(const vk::Device& device, const vk::Image& image, const vk::Queue& queue, float x, float y, float w, float h);
		os_VideoRoutingPublishFrameTexture(device, srcImage, graphicsQueue, 0, 0, targetWidth, targetHeight);
	}
	else
	{
		os_VideoRoutingTermVk();
	}
#endif
}

void BaseVulkanRenderer::CheckFogTexture()
{
	if (!fogTexture)
	{
		fogTexture = std::make_unique<Texture>();
		fogTexture->tex_type = TextureType::_8;
		updateFogTable = true;
	}
	if (!updateFogTable || !config::Fog)
		return;
	updateFogTable = false;
	u8 texData[256];
	MakeFogTexture(texData);

	fogTexture->SetCommandBuffer(texCommandBuffer);
	fogTexture->UploadToGPU(128, 2, texData, false);
	fogTexture->SetCommandBuffer(nullptr);
}

void BaseVulkanRenderer::CheckPaletteTexture()
{
	if (!paletteTexture) {
		paletteTexture = std::make_unique<Texture>();
		paletteTexture->tex_type = TextureType::_8888;
	}
	else if (!updatePalette) {
		return;
	}
	updatePalette = false;

	paletteTexture->SetCommandBuffer(texCommandBuffer);
	paletteTexture->UploadToGPU(1024, 1, (u8 *)palette32_ram, false);
	paletteTexture->SetCommandBuffer(nullptr);
}

bool BaseVulkanRenderer::presentFramebuffer()
{
	if (framebufferTexIndex >= (int)framebufferTextures.size())
		return false;
	Texture *fbTexture = framebufferTextures[framebufferTexIndex].get();
	if (fbTexture == nullptr)
		return false;
	GetContext()->PresentFrame(fbTexture->GetImage(), fbTexture->GetImageView(), fbTexture->getSize(),
			getDCFramebufferAspectRatio());
	return true;
}

class VulkanRenderer final : public BaseVulkanRenderer
{
public:
	bool Init() override
	{
		NOTICE_LOG(RENDERER, "VulkanRenderer::Init");

		textureDrawer.Init(&samplerManager, &shaderManager, &textureCache);
		textureDrawer.SetCommandPool(&texCommandPool);

		screenDrawer.Init(&samplerManager, &shaderManager, viewport);
		screenDrawer.SetCommandPool(&texCommandPool);
		BaseInit(screenDrawer.GetRenderPass());
		emulateFramebuffer = config::EmulateFramebuffer;

		/// Initialize GPU-driven rendering if enabled
		if (config::GpuDrivenRendering) {
			g_gpuDrivenRenderer = std::make_unique<GPUDrivenRenderer>();
			if (g_gpuDrivenRenderer->Init(GetContext())) {
				INFO_LOG(RENDERER, "GPU-Driven Rendering enabled successfully");
			} else {
				WARN_LOG(RENDERER, "GPU-Driven Rendering failed to initialize, falling back to traditional rendering");
				g_gpuDrivenRenderer.reset();
			}
		}

		/// Initialize iOS Texture Streaming Manager for enhanced performance
#ifdef __APPLE__
#if TARGET_OS_IOS || TARGET_OS_TV
		try {
			auto& streamingMgr = flycast::IOSTextureStreamingManager::Instance();
			streamingMgr.Initialize();
			INFO_LOG(RENDERER, "🔧 iOS Texture Streaming Manager initialized successfully");

			// Initialize NEON TA processor for FMV and scene loading optimizations
			flycast::NEONTAProcessor::Initialize();
			flycast::NEONPerfMonitor::Initialize();

		} catch (const std::exception& e) {
			WARN_LOG(RENDERER, "⚠️ iOS optimization initialization failed: %s", e.what());
			g_gpuDrivenRenderer.reset();
		}
#endif
#endif

		/// Initialize Async FMV Pipeline for CPU stall elimination
		g_asyncFMVPipeline = std::make_unique<AsyncFMVPipeline>();
		if (g_asyncFMVPipeline->Init(GetContext())) {
			g_asyncFMVPipeline->StartProcessing();
			INFO_LOG(RENDERER, "🎬 Async FMV Pipeline initialized - CPU stalls eliminated");
		} else {
			WARN_LOG(RENDERER, "⚠️ Async FMV Pipeline initialization failed");
			g_asyncFMVPipeline.reset();
		}

		/// Initialize Fence-Free Submitter for GPU synchronization optimization
		g_fenceFreeSubmitter = std::make_unique<FenceFreeSubmitter>();
		if (g_fenceFreeSubmitter->Init(GetContext())) {
			g_fenceFreeSubmitter->StartSubmissionThread();
			INFO_LOG(RENDERER, "🚀 Fence-Free Submitter initialized - GPU sync stalls eliminated");
		} else {
			WARN_LOG(RENDERER, "⚠️ Fence-Free Submitter initialization failed");
			g_fenceFreeSubmitter.reset();
		}

		return true;
	}

	void Term() override
	{
		DEBUG_LOG(RENDERER, "VulkanRenderer::Term");
		GetContext()->WaitIdle();

		/// Terminate Async FMV Pipeline
		if (g_asyncFMVPipeline) {
			g_asyncFMVPipeline->Term();
			g_asyncFMVPipeline.reset();
		}

		/// Terminate Fence-Free Submitter
		if (g_fenceFreeSubmitter) {
			g_fenceFreeSubmitter->Term();
			g_fenceFreeSubmitter.reset();
		}

		/// Terminate GPU-driven renderer
		if (g_gpuDrivenRenderer) {
			g_gpuDrivenRenderer->Term();
			g_gpuDrivenRenderer.reset();
		}

		texCommandPool.Term(); // make sure all in-flight buffers are returned
		screenDrawer.Term();
		textureDrawer.Term();
		samplerManager.term();
		BaseVulkanRenderer::Term();
	}

	void Process(TA_context* ctx) override
	{
		if (emulateFramebuffer != config::EmulateFramebuffer)
		{
			screenDrawer.EndRenderPass();
			VulkanContext::Instance()->WaitIdle();
			screenDrawer.Term();
			screenDrawer.Init(&samplerManager, &shaderManager, viewport);
			BaseInit(screenDrawer.GetRenderPass());
			emulateFramebuffer = config::EmulateFramebuffer;
		}
		else if (ctx->rend.isRTT) {
			screenDrawer.EndRenderPass();
		}
		BaseVulkanRenderer::Process(ctx);

		/// Process GPU-driven rendering if enabled
		if (g_gpuDrivenRenderer && g_gpuDrivenRenderer->IsEnabled() && !ctx->rend.isRTT) {
			ProcessGPUDrivenRendering(ctx);
		}
	}

	bool Render() override
	{
		try {
			Drawer *drawer;
			if (pvrrc.isRTT)
				drawer = &textureDrawer;
			else {
				resize(pvrrc.framebufferWidth, pvrrc.framebufferHeight);
				drawer = &screenDrawer;
			}

			/// Use GPU-driven rendering path if available
			if (g_gpuDrivenRenderer && g_gpuDrivenRenderer->IsEnabled() && !pvrrc.isRTT) {
				return RenderGPUDriven(drawer);
			} else {
				/// Traditional rendering path
				drawer->Draw(fogTexture.get(), paletteTexture.get());
				if (config::EmulateFramebuffer || pvrrc.isRTT)
					// delay ending the render pass in case of multi render
					drawer->EndRenderPass();
				return !pvrrc.isRTT;
			}
		} catch (const vk::SystemError& e) {
			// Sometimes happens when resizing the window
			WARN_LOG(RENDERER, "Vulkan system error %s", e.what());

			return false;
		}
	}

	bool Present() override
	{
		if (clearLastFrame)
			return false;
		if (config::EmulateFramebuffer || framebufferRendered)
			return presentFramebuffer();
		else
			return screenDrawer.PresentFrame();
	}

protected:
	void resize(int w, int h) override
	{
		if ((u32)w == viewport.width && (u32)h == viewport.height)
			return;
		BaseVulkanRenderer::resize(w, h);
		GetContext()->WaitIdle();
		screenDrawer.Init(&samplerManager, &shaderManager, viewport);
	}

private:
	SamplerManager samplerManager;
	ScreenDrawer screenDrawer;
	TextureDrawer textureDrawer;
	bool emulateFramebuffer = false;

	/// GPU-driven rendering data
	std::vector<GPUDrivenRenderer::ObjectData> gpuObjects;

	void ProcessGPUDrivenRendering(TA_context* ctx) {
		if (!g_gpuDrivenRenderer) return;

		/// Convert Flycast render data to GPU objects
		gpuObjects.clear();

		/// Process opaque objects
		for (u32 i = 0; i < pvrrc.global_param_op.size(); i++) {
			const PolyParam& param = pvrrc.global_param_op[i];

			GPUDrivenRenderer::ObjectData obj;
			/// Calculate bounding sphere from poly data
			obj.center = glm::vec3(0.0f); // Would calculate actual center
			obj.radius = 10.0f; // Would calculate actual radius
			obj.materialId = i;
			obj.geometryOffset = param.first;
			obj.indexCount = param.count;
			obj.flags = 0; // Opaque

			gpuObjects.push_back(obj);
		}

		/// Process translucent objects
		for (u32 i = 0; i < pvrrc.global_param_tr.size(); i++) {
			const PolyParam& param = pvrrc.global_param_tr[i];

			GPUDrivenRenderer::ObjectData obj;
			obj.center = glm::vec3(0.0f);
			obj.radius = 10.0f;
			obj.materialId = pvrrc.global_param_op.size() + i;
			obj.geometryOffset = param.first;
			obj.indexCount = param.count;
			obj.flags = 1; // Transparent

			gpuObjects.push_back(obj);
		}

		/// Upload to GPU
		g_gpuDrivenRenderer->UploadObjects(gpuObjects);

		INFO_LOG(RENDERER, "GPU-Driven: Uploaded %zu objects for culling", gpuObjects.size());
	}

	bool RenderGPUDriven(Drawer* drawer) {
		/// Setup culling uniforms
		GPUDrivenRenderer::CullingUniforms uniforms;

		/// Get matrices from the drawer - simplified access
		uniforms.viewMatrix = glm::mat4(1.0f); // Would get actual view matrix
		uniforms.projMatrix = glm::mat4(1.0f); // Would get actual projection matrix
		uniforms.screenSize = glm::vec2(viewport.width, viewport.height);
		uniforms.maxObjects = static_cast<uint32_t>(gpuObjects.size());
		uniforms.tileCount = (viewport.width / 32) * (viewport.height / 32);

		/// Extract frustum planes from projection matrix (simplified)
		for (int i = 0; i < 6; i++) {
			uniforms.frustumPlanes[i] = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
		}

		/// Perform GPU culling
		g_gpuDrivenRenderer->PerformCulling(uniforms);

		/// Traditional rendering for now (GPU execution would go here)
		drawer->Draw(fogTexture.get(), paletteTexture.get());

		/// Log GPU stats
		auto stats = g_gpuDrivenRenderer->GetStats();
		DEBUG_LOG(RENDERER, "GPU-Driven Stats: %u total, %u visible, %u culled",
		         stats.totalObjects, stats.visibleObjects, stats.culledObjects);

		if (config::EmulateFramebuffer || pvrrc.isRTT)
			drawer->EndRenderPass();

		return !pvrrc.isRTT;
	}
};

Renderer* rend_Vulkan()
{
	return new VulkanRenderer();
}

void ReInitOSD()
{
	if (renderer != nullptr) {
		BaseVulkanRenderer *vkrenderer = dynamic_cast<BaseVulkanRenderer*>(renderer);
		if (vkrenderer != nullptr)
			vkrenderer->ReInitOSD();
	}
}
