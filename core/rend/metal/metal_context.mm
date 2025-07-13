/*
    Copyright 2025 flyinghead

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

#include "metal_context.h"
#include "cfg/option.h"
#ifndef LIBRETRO
#include "metal_driver.h"
#endif
#ifdef USE_SDL
#include "sdl/sdl.h"
#endif
#ifndef LIBRETRO
#include "ui/imgui_driver.h"
#endif
#import "metal_buffer.h"

MetalContext *MetalContext::contextInstance;

void MetalContext::CreateSwapChain()
{
    // WAIT IDLE

    commandBuffers.clear();

#ifdef LIBRETRO
    // In libretro mode, we don't manage the layer
    // Dimensions are set via UpdateLayerSize() calls from the frontend
    // Validate dimensions - ensure they're reasonable for rendering
    if (width == 0 || height == 0) {
        WARN_LOG(RENDERER, "Metal context has invalid size: %dx%d, using fallback", (int)width, (int)height);
        width = 640;
        height = 480;
    }
#else
    [layer setPixelFormat:MTLPixelFormatBGRA8Unorm];
    [layer setFramebufferOnly:TRUE];
    [layer setColorspace:CGColorSpaceCreateWithName(kCGColorSpaceSRGB)];
    [layer setMaximumDrawableCount:3];
#if TARGET_OS_MAC || TARGET_OS_MACCATALYST
#if TARGET_OS_OSX
    [layer setDisplaySyncEnabled:TRUE];
#endif
#endif

    auto size = [layer drawableSize];
    width = size.width;
    height = size.height;

    // Validate dimensions - ensure they're reasonable for rendering
    if (width == 0 || height == 0) {
        WARN_LOG(RENDERER, "Metal layer has invalid size: %dx%d, using fallback", (int)width, (int)height);
        width = 640;
        height = 480;
        // Update the layer with our fallback size
        layer.drawableSize = CGSizeMake(width, height);
    }
#endif

    SetWindowSize(width, height);
    resized = false;

    // Note: DupeFrames feature disabled for Metal renderer to avoid linking issues
    // TODO: Re-enable once symbol visibility between ObjC++ and C++ is resolved
    if (false && swapOnVSync && settings.display.refreshRate > 60.f)
        swapInterval = settings.display.refreshRate / 60.f;
    else
        swapInterval = 1;

    commandBuffers.resize(3);

    quadPipeline->Init(shaderManager.get());
    quadPipelineWithAlpha->Init(shaderManager.get());
    quadDrawer->Init(quadPipeline.get());
    quadRotatePipeline->Init(shaderManager.get());
    quadRotateDrawer->Init(quadRotatePipeline.get());

    currentImage = 2;

    ERROR_LOG(RENDERER, "Metal swap chain created: %d x %d, swap chain size %d", width, height, 3);
}

bool MetalContext::init()
{
    GraphicsContext::instance = this;

#ifdef USE_SDL
    if (!sdl_recreate_window(SDL_WINDOW_METAL))
        return false;

    auto view = SDL_Metal_CreateView((SDL_Window *)window);

    if (view == nullptr) {
        term();
        ERROR_LOG(RENDERER, "Failed to create SDL Metal View");
        return false;
    }

    layer = static_cast<CAMetalLayer*>(SDL_Metal_GetLayer(view));
#endif

#ifdef LIBRETRO
    // In libretro mode, the Metal layer is managed by the frontend's Metal video driver
    // We don't create or manage the layer directly - we just work with the device
    layer = nil;

    // Set default dimensions that will be updated when the frontend provides proper dimensions
    width = 640;
    height = 480;

    NOTICE_LOG(RENDERER, "Metal context initialized for libretro mode (no layer management)");
#endif

    device = MTLCreateSystemDefaultDevice();

    if (!device) {
        term();
        NOTICE_LOG(RENDERER, "Metal Device is null.");
        return false;
    }

    if (layer != nil)
        [layer setDevice:device];
    queue = [device newCommandQueue];

    shaderManager = std::make_unique<MetalShaders>();
    quadPipeline = std::make_unique<MetalQuadPipeline>(true, false);
    quadPipelineWithAlpha = std::make_unique<MetalQuadPipeline>(false, false);
    quadDrawer = std::make_unique<MetalQuadDrawer>();
    quadRotatePipeline = std::make_unique<MetalQuadPipeline>(true, true);
    quadRotateDrawer = std::make_unique<MetalQuadDrawer>();

    NOTICE_LOG(RENDERER, "Created Metal view.");

    #ifndef LIBRETRO
        imguiDriver = std::unique_ptr<ImGuiDriver>(new MetalDriver());
#endif

    CreateSwapChain();

    return true;
}

std::string MetalContext::getDriverName() {
    return [[device name] UTF8String];
}

bool MetalContext::recreateSwapChainIfNeeded()
{
    if (resized || HasSurfaceDimensionChanged())
    {
        CreateSwapChain();
        lastFrameTexture = nil;
        return true;
    }
    else
        return false;
}

void MetalContext::BeginRenderPass() {
    recreateSwapChainIfNeeded();
    if (!IsValid())
        return;

#ifdef LIBRETRO
    // In libretro mode, we create our own render target textures since libretro manages presentation
    // Set up command buffers and encoders for internal rendering operations

    if (!renderPassDescriptor) {
        renderPassDescriptor = [[MTLRenderPassDescriptor alloc] init];
    }

    // Create or reuse offscreen render target texture for libretro
    if (!offscreenTexture || [offscreenTexture width] != width || [offscreenTexture height] != height) {
        MTLTextureDescriptor *textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                                       width:width
                                                                                                      height:height
                                                                                                   mipmapped:NO];
        textureDescriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        offscreenTexture = [device newTextureWithDescriptor:textureDescriptor];
    }

    auto colorAttachment = renderPassDescriptor.colorAttachments[0];
    [colorAttachment setTexture:offscreenTexture];
    [colorAttachment setLoadAction:MTLLoadActionClear];
    [colorAttachment setStoreAction:MTLStoreActionStore];
    [colorAttachment setClearColor:MTLClearColorMake(VO_BORDER_COL.red(), VO_BORDER_COL.green(), VO_BORDER_COL.blue(), 1.0f)];

    if (currentImage >= commandBuffers.size()) {
        commandBuffers.resize(currentImage + 1);
    }

    if (!commandBuffers[currentImage]) {
        commandBuffers[currentImage] = [queue commandBuffer];
        [commandBuffers[currentImage] setLabel:@"Render Frame (libretro)"];
    }

    commandEncoder = [commandBuffers[currentImage] renderCommandEncoderWithDescriptor: renderPassDescriptor];
    // Note: No presentDrawable in libretro mode - we'll copy to libretro's framebuffer later

#else
    currentDrawable = [layer nextDrawable];

    if (!renderPassDescriptor) {
        renderPassDescriptor = [[MTLRenderPassDescriptor alloc] init];
    }

    auto colorAttachment = renderPassDescriptor.colorAttachments[0];
    [colorAttachment setTexture:currentDrawable.texture];
    [colorAttachment setLoadAction:MTLLoadActionClear];
    [colorAttachment setStoreAction:MTLStoreActionStore];
    [colorAttachment setClearColor:MTLClearColorMake(VO_BORDER_COL.red(), VO_BORDER_COL.green(), VO_BORDER_COL.blue(), 1.0f)];

    if (currentImage >= commandBuffers.size()) {
        commandBuffers.resize(currentImage + 1);
    }

    if (!commandBuffers[currentImage]) {
        commandBuffers[currentImage] = [queue commandBuffer];
        [commandBuffers[currentImage] setLabel:@"Render Frame"];
    }

    commandEncoder = [commandBuffers[currentImage] renderCommandEncoderWithDescriptor: renderPassDescriptor];
    [commandBuffers[currentImage] presentDrawable:currentDrawable];
#endif
};

void MetalContext::NewFrame() {
    if (!IsValid())
        return;

    currentImage = (currentImage + 1) % 3;
    currentDrawable = nil;
    verify(!rendering);
    rendering = true;
}

void MetalContext::EndFrame() {
    if (!IsValid())
        return;

#ifdef LIBRETRO
    // In libretro mode, we still need to properly end the render pass and commit
    // But we don't present to a drawable since libretro handles presentation
    if (commandEncoder != nil) {
        [commandEncoder endEncoding];
        commandEncoder = nil;
    }

    if (commandBuffers[currentImage] != nil) {
        [commandBuffers[currentImage] commit];
        [commandBuffers[currentImage] waitUntilCompleted];
        commandBuffers[currentImage] = nil;
    }

    verify(rendering);
    rendering = false;
    renderDone = true;
    return;
#endif

    [commandEncoder endEncoding];
    [commandBuffers[currentImage] commit];
    [commandBuffers[currentImage] waitUntilCompleted];
    commandBuffers[currentImage] = nil;

    verify(rendering);
    rendering = false;
    renderDone = true;
}

void MetalContext::Present()
{
    if (renderDone)
    {
#ifndef LIBRETRO
        if (lastFrameTexture != nil && IsValid() && !gui_is_open())
#else
        if (lastFrameTexture != nil && IsValid())
#endif
            for (int i = 1; i < swapInterval; i++)
            {
                PresentFrame(lastFrameTexture, lastFrameViewport, lastFrameAR);
            }
        renderDone = false;
    }
    if (swapOnVSync == (settings.input.fastForwardMode || !config::VSync))
    {
        swapOnVSync = (!settings.input.fastForwardMode && config::VSync);
        resized = true;
    }
    if (resized) {
        CreateSwapChain();
        lastFrameTexture = nil;
    }
}

void MetalContext::DrawFrame(id<MTLTexture> texture, MTLViewport viewport, float aspectRatio) {
#ifdef LIBRETRO
    // In libretro mode, we render to our offscreen texture instead of the layer's drawable
    // The frontend will handle the final presentation
    if (offscreenTexture == nil || commandEncoder == nil) {
        DEBUG_LOG(RENDERER, "Skipping DrawFrame - offscreen texture or encoder not available");
        return;
    }
#endif

    MetalQuadVertex vtx[4] {
            { -1, -1, 0, 0, 1 },
            {  1, -1, 0, 1, 1 },
            { -1,  1, 0, 0, 0 },
            {  1,  1, 0, 1, 0 },
    };
    float shiftX, shiftY;
    getVideoShift(shiftX, shiftY);
    vtx[0].x = vtx[2].x = -1.f + shiftX * 2.f / viewport.width;
    vtx[1].x = vtx[3].x = vtx[0].x + 2;
    vtx[0].y = vtx[1].y = -1.f + shiftY * 2.f / viewport.height;
    vtx[2].y = vtx[3].y = vtx[0].y + 2;

    [commandEncoder pushDebugGroup:@"DrawFrame"];

    if (config::Rotate90)
        quadRotatePipeline->BindPipeline(commandEncoder);
    else
        quadPipeline->BindPipeline(commandEncoder);

    float screenAR = (float)width / height;
    float dx = 0;
    float dy = 0;
    if (aspectRatio > screenAR)
        dy = height * (1 - screenAR / aspectRatio) / 2;
    else
        dx = width * (1 - aspectRatio / screenAR) / 2;

    MTLViewport framePort = { dx, dy, width - dx * 2, height - dy * 2, 0, 1 };
    [commandEncoder setViewport:framePort];

    // Apply bounds checking to prevent Metal validation errors
    MTLScissorRect scissor = MTLScissorRect { (uint)dx, (uint)dy, (uint)(width - dx * 2), (uint)(height - dy * 2) };

    // Clamp scissor to viewport bounds
    scissor.x = std::min(scissor.x, (NSUInteger)width);
    scissor.y = std::min(scissor.y, (NSUInteger)height);

    if (scissor.x + scissor.width > width) {
        scissor.width = width - scissor.x;
    }
    if (scissor.y + scissor.height > height) {
        scissor.height = height - scissor.y;
    }

    // Ensure minimum size
    scissor.width = std::max(scissor.width, (NSUInteger)1);
    scissor.height = std::max(scissor.height, (NSUInteger)1);

    [commandEncoder setScissorRect:scissor];

    if (config::Rotate90)
        quadRotateDrawer->Draw(commandEncoder, texture, vtx, config::TextureFiltering == 1);
    else
        quadDrawer->Draw(commandEncoder, texture, vtx, config::TextureFiltering == 1);

    [commandEncoder popDebugGroup];
}

void MetalContext::PresentFrame(id<MTLTexture> texture, MTLViewport viewport, float aspectRatio)
{
    lastFrameTexture = texture;
    lastFrameViewport = viewport;
    lastFrameAR = aspectRatio;

    if (texture != nil && IsValid())
    {
        NewFrame();

        BeginRenderPass();

#ifndef LIBRETRO
        gui_draw_osd();
#endif

        if (lastFrameTexture != nil) // Might have been nullified if swap chain recreated
            DrawFrame(texture, viewport, aspectRatio);

    #ifndef LIBRETRO
    imguiDriver->renderDrawData(ImGui::GetDrawData(), false);
#endif
        EndFrame();
    }
    else {
        if (!IsValid())
        {
            DEBUG_LOG(RENDERER, "Skipping Metal presentation - invalid size: %dx%d", width, height);
        }
        else if (texture == nil)
        {
            DEBUG_LOG(RENDERER, "Skipping Metal presentation - no texture provided");
        }
    }
}

void MetalContext::PresentLastFrame()
{
    if (lastFrameTexture != nil && IsValid())
        DrawFrame(lastFrameTexture, lastFrameViewport, lastFrameAR);
}

void MetalContext::term() {
    GraphicsContext::instance = nullptr;
    lastFrameTexture = nil;
#ifndef LIBRETRO
    imguiDriver.reset();
#endif
    quadDrawer.reset();
    quadPipeline.reset();
    quadPipelineWithAlpha.reset();
    quadRotateDrawer.reset();
    quadRotatePipeline.reset();
    shaderManager.reset();
    commandBuffers.clear();
}

bool MetalContext::HasSurfaceDimensionChanged() const
{
#ifdef LIBRETRO
    // In libretro mode, we don't have a layer, so surface dimensions are managed externally
    return false;
#endif
    auto size = [layer drawableSize];
    return width != size.width || height != size.height;
}

void MetalContext::SetWindowSize(u32 width, u32 height)
{
    if (this->width != width || this->height != height)
    {
        // Validate dimensions to prevent crashes with invalid values
        if (width == 0 || height == 0) {
            WARN_LOG(RENDERER, "Invalid window size: %dx%d, ignoring", width, height);
            return;
        }

        // Prevent extremely large dimensions that could cause memory issues
        if (width > 16384 || height > 16384) {
            WARN_LOG(RENDERER, "Extremely large window size: %dx%d, clamping", width, height);
            width = std::min(width, 16384u);
            height = std::min(height, 16384u);
        }

        DEBUG_LOG(RENDERER, "Metal context size change: %dx%d -> %dx%d", this->width, this->height, width, height);

        this->width = width;
        this->height = height;

        if (width != 0)
            settings.display.width = width;

        if (height != 0)
            settings.display.height = height;

        resize();
    }
}

#ifdef LIBRETRO
void MetalContext::UpdateLayerSize(u32 width, u32 height)
{
    if (width == 0 || height == 0) {
        WARN_LOG(RENDERER, "Ignoring invalid layer size update: %dx%d", width, height);
        return;
    }

    NOTICE_LOG(RENDERER, "Updating Metal context dimensions to %dx%d", width, height);

    // In libretro mode, we don't manage the Metal layer directly
    // The frontend's Metal video driver handles the layer
    // We just update our internal dimensions

    // Update context dimensions
    this->width = width;
    this->height = height;

    // Update display settings
    if (width != 0)
        settings.display.width = width;
    if (height != 0)
        settings.display.height = height;

    // Mark as needing recreation for next frame
    resized = true;
}
#endif

bool MetalContext::GetLastFrame(std::vector<u8> &data, int &width, int &height)
{
    if (lastFrameTexture == nil)
        return false;

    if (width != 0) {
        height = width / lastFrameAR;
    }
    else if (height != 0) {
        width = lastFrameAR * height;
    }
    else
    {
        width = lastFrameViewport.width;
        height = lastFrameViewport.height;
        if (config::Rotate90)
            std::swap(width, height);
        // We need square pixels for PNG
        int w = lastFrameAR * height;
        if (width > w)
            height = width / lastFrameAR;
        else
            width = w;
    }

    MTLTextureDescriptor *renderTargetDesc = [[MTLTextureDescriptor alloc] init];
    renderTargetDesc.width = width;
    renderTargetDesc.height = height;
    renderTargetDesc.pixelFormat = MTLPixelFormatRGBA8Unorm;
    renderTargetDesc.usage = MTLTextureUsageRenderTarget;
    renderTargetDesc.storageMode = MTLStorageModePrivate;

    id<MTLTexture> renderTarget = [device newTextureWithDescriptor:renderTargetDesc];
    [renderTarget setLabel:@"Screenshot Render Target"];

    NSUInteger bytesPerPixel = 4;
    NSUInteger bytesPerRow = width * bytesPerPixel;
    NSUInteger bufferSize = bytesPerRow * height;

    id<MTLBuffer> readbackBuffer = [device newBufferWithLength:bufferSize
                                                       options:MTLResourceStorageModeShared];
    [readbackBuffer setLabel:@"Screenshot Readback Buffer"];

    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    [commandBuffer setLabel:@"GetLastFrame"];

    MTLRenderPassDescriptor *renderPassDesc = [[MTLRenderPassDescriptor alloc] init];
    renderPassDesc.colorAttachments[0].texture = renderTarget;
    renderPassDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
    renderPassDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
    renderPassDesc.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

    id<MTLRenderCommandEncoder> renderEncoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPassDesc];
    [renderEncoder setLabel:@"GetLastFrame Render"];

    MTLViewport viewport = {
            0.0, 0.0,
            (double)width, (double)height,
            0.0, 1.0
    };
    [renderEncoder setViewport:viewport];

    // Apply bounds checking to prevent Metal validation errors
    MTLScissorRect scissor = {
            0, 0,
            (NSUInteger)width, (NSUInteger)height
    };

    // Basic validation - ensure scissor doesn't exceed render target bounds
    scissor.width = std::min(scissor.width, (NSUInteger)width);
    scissor.height = std::min(scissor.height, (NSUInteger)height);
    scissor.width = std::max(scissor.width, (NSUInteger)1);
    scissor.height = std::max(scissor.height, (NSUInteger)1);

    [renderEncoder setScissorRect:scissor];

    MetalQuadVertex vtx[4] = {
            { -1.f, -1.f, 0.f, 0.f, 1.f },
            {  1.f, -1.f, 0.f, 1.f, 1.f },
            { -1.f,  1.f, 0.f, 0.f, 0.f },
            {  1.f,  1.f, 0.f, 1.f, 0.f },
    };

    if (config::Rotate90) {
        quadRotatePipeline->BindPipeline(renderEncoder);
        quadRotateDrawer->Draw(renderEncoder, lastFrameTexture, vtx, false);
    } else {
        quadPipeline->BindPipeline(renderEncoder);
        quadDrawer->Draw(renderEncoder, lastFrameTexture, vtx, false);
    }

    [renderEncoder endEncoding];

    // Copy from render target to buffer
    id<MTLBlitCommandEncoder> blitEncoder = [commandBuffer blitCommandEncoder];
    [blitEncoder setLabel:@"GetLastFrame Blit"];

    MTLOrigin sourceOrigin = MTLOriginMake(0, 0, 0);
    MTLSize sourceSize = MTLSizeMake(width, height, 1);

    [blitEncoder copyFromTexture:renderTarget
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:sourceOrigin
                      sourceSize:sourceSize
                        toBuffer:readbackBuffer
               destinationOffset:0
          destinationBytesPerRow:bytesPerRow
        destinationBytesPerImage:bufferSize];

    [blitEncoder endEncoding];

    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];

    if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
        NSError *error = commandBuffer.error;
        WARN_LOG(RENDERER, "MetalContext::GetLastFrame: Command buffer failed: %s",
                 error ? error.localizedDescription.UTF8String : "Unknown error");
        return false;
    }

    // Read back the data
    const u8 *img = (const u8 *)[readbackBuffer contents];
    data.clear();

    data.reserve(width * height * 3);
    // RGBA -> RGB conversion
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            data.push_back(*img++); // R
            data.push_back(*img++); // G
            data.push_back(*img++); // B
            img++; // Skip A
        }
    }

    return true;
}

#ifdef LIBRETRO
bool MetalContext::GetFrameBuffer(std::vector<u32>& data, int width, int height)
{
    if (offscreenTexture == nil || !IsValid())
        return false;

    NSUInteger bytesPerPixel = 4;  // BGRA
    NSUInteger bytesPerRow = width * bytesPerPixel;
    NSUInteger bufferSize = bytesPerRow * height;

    id<MTLBuffer> readbackBuffer = [device newBufferWithLength:bufferSize
                                                       options:MTLResourceStorageModeShared];
    [readbackBuffer setLabel:@"Libretro Frame Readback Buffer"];

    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    [commandBuffer setLabel:@"GetFrameBuffer"];

    // Copy from our offscreen texture to buffer
    id<MTLBlitCommandEncoder> blitEncoder = [commandBuffer blitCommandEncoder];
    [blitEncoder setLabel:@"GetFrameBuffer Blit"];

    MTLOrigin sourceOrigin = MTLOriginMake(0, 0, 0);
    MTLSize sourceSize = MTLSizeMake(width, height, 1);

    [blitEncoder copyFromTexture:offscreenTexture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:sourceOrigin
                      sourceSize:sourceSize
                        toBuffer:readbackBuffer
               destinationOffset:0
          destinationBytesPerRow:bytesPerRow
        destinationBytesPerImage:bufferSize];

    [blitEncoder endEncoding];

    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];

    if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
        NSError *error = commandBuffer.error;
        WARN_LOG(RENDERER, "MetalContext::GetFrameBuffer: Command buffer failed: %s",
                 error ? error.localizedDescription.UTF8String : "Unknown error");
        return false;
    }

    // Read back the data (already in BGRA format)
    const u32 *pixels = (const u32 *)[readbackBuffer contents];
    data.clear();
    data.reserve(width * height);

    for (int i = 0; i < width * height; i++) {
        data.push_back(pixels[i]);
    }

    return true;
}
#endif

MetalContext::MetalContext() {
    verify(contextInstance == nullptr);
    contextInstance = this;
}

MetalContext::~MetalContext() {
    verify(contextInstance == this);
    contextInstance = nullptr;
}
