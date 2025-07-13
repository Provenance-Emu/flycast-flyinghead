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

#include "metal_drawer.h"
#include <cmath>

TileClipping MetalBaseDrawer::SetTileClip(id<MTLRenderCommandEncoder> encoder, u32 val, MTLScissorRect& clipRect) {
    int rect[4] = {};
    TileClipping clipMode = GetTileClip(val, matrices.GetViewportMatrix(), rect);
    if (clipMode != TileClipping::Off)
    {
        clipRect.x = rect[0];
        clipRect.y = rect[1];
        clipRect.width = rect[2];
        clipRect.height = rect[3];
    }
    if (clipMode == TileClipping::Outside)
        SetScissor(encoder, clipRect);
    else
        SetScissor(encoder, baseScissor);

    return clipMode;
}

void MetalBaseDrawer::SetBaseScissor(MTLViewport viewport) {
    INFO_LOG(RENDERER, "SetBaseScissor called with viewport: %.0fx%.0f at (%.0f,%.0f)",
             viewport.width, viewport.height, viewport.originX, viewport.originY);

    bool wide_screen_on = config::Widescreen
                          && !matrices.IsClipped() && !config::Rotate90 && !config::EmulateFramebuffer;
    INFO_LOG(RENDERER, "Wide screen mode: %s (Widescreen=%d, IsClipped=%d, Rotate90=%d, EmulateFramebuffer=%d)",
             wide_screen_on ? "ON" : "OFF", (int)(bool)config::Widescreen, matrices.IsClipped(), (int)(bool)config::Rotate90, (int)(bool)config::EmulateFramebuffer);

    // Log hardware scaler settings
    INFO_LOG(RENDERER, "Hardware scaler: vscalefactor=0x%X (%d), hscale=%d, render target height=%.0f",
             pvrrc.scaler_ctl.vscalefactor, pvrrc.scaler_ctl.vscalefactor, pvrrc.scaler_ctl.hscale, viewport.height);

    if (!wide_screen_on)
    {
        float width;
        float height;
        float min_x;
        float min_y;
        glm::vec4 clip_min(pvrrc.fb_X_CLIP.min, pvrrc.fb_Y_CLIP.min, 0, 1);
        glm::vec4 clip_dim(pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1,
                           pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1, 0, 0);

        INFO_LOG(RENDERER, "Framebuffer clips: X=(%d-%d), Y=(%d-%d)",
                 pvrrc.fb_X_CLIP.min, pvrrc.fb_X_CLIP.max, pvrrc.fb_Y_CLIP.min, pvrrc.fb_Y_CLIP.max);
        INFO_LOG(RENDERER, "Before matrix transform: clip_min=(%.1f,%.1f), clip_dim=(%.1f,%.1f)",
                 clip_min[0], clip_min[1], clip_dim[0], clip_dim[1]);

        clip_min = matrices.GetScissorMatrix() * clip_min;
        clip_dim = matrices.GetScissorMatrix() * clip_dim;

        INFO_LOG(RENDERER, "After matrix transform: clip_min=(%.1f,%.1f), clip_dim=(%.1f,%.1f)",
                 clip_min[0], clip_min[1], clip_dim[0], clip_dim[1]);

        min_x = clip_min[0];
        min_y = clip_min[1];
        width = clip_dim[0];
        height = clip_dim[1];
        if (width < 0)
        {
            min_x += width;
            width = -width;
            INFO_LOG(RENDERER, "Negative width corrected: min_x=%.1f, width=%.1f", min_x, width);
        }
        if (height < 0)
        {
            min_y += height;
            height = -height;
            INFO_LOG(RENDERER, "Negative height corrected: min_y=%.1f, height=%.1f", min_y, height);
        }

        baseScissor = MTLScissorRect();
        baseScissor.x = std::max(lroundf(min_x), 0L);
        baseScissor.y = std::max(lroundf(min_y), 0L);
        baseScissor.width = std::max(lroundf(width), 0L);
        baseScissor.height = std::max(lroundf(height), 0L);

        INFO_LOG(RENDERER, "Calculated baseScissor: (%d,%d,%d,%d) from (%.1f,%.1f,%.1f,%.1f)",
                 (int)baseScissor.x, (int)baseScissor.y, (int)baseScissor.width, (int)baseScissor.height,
                 min_x, min_y, width, height);

        // CRITICAL FIX: Clamp baseScissor to viewport/render target bounds to prevent Metal validation errors
        // The hardware scaler can apply scaling factors that result in logical scissor rects larger than
        // the actual render target (e.g., 2x vertical scaling making 480->960), but Metal requires
        // scissor rects to be within the physical render target bounds.
        NSUInteger maxWidth = (NSUInteger)viewport.width;
        NSUInteger maxHeight = (NSUInteger)viewport.height;

        baseScissor.x = std::min(baseScissor.x, maxWidth);
        baseScissor.y = std::min(baseScissor.y, maxHeight);

        if (baseScissor.x + baseScissor.width > maxWidth) {
            baseScissor.width = maxWidth - baseScissor.x;
        }
        if (baseScissor.y + baseScissor.height > maxHeight) {
            baseScissor.height = maxHeight - baseScissor.y;
        }

        // Ensure minimum dimensions
        baseScissor.width = std::max(baseScissor.width, (NSUInteger)1);
        baseScissor.height = std::max(baseScissor.height, (NSUInteger)1);

        INFO_LOG(RENDERER, "Clamped baseScissor to render target bounds: (%d,%d,%d,%d)",
                 (int)baseScissor.x, (int)baseScissor.y, (int)baseScissor.width, (int)baseScissor.height);
    }
    else
    {
        baseScissor = MTLScissorRect();
        baseScissor.x = 0;
        baseScissor.y = 0;
        baseScissor.width = viewport.width;
        baseScissor.height = viewport.height;

        INFO_LOG(RENDERER, "Wide screen baseScissor: (%d,%d,%d,%d)",
                 (int)baseScissor.x, (int)baseScissor.y, (int)baseScissor.width, (int)baseScissor.height);
        INFO_LOG(RENDERER, "Hardware scaler in widescreen mode: vscalefactor=0x%X (%d), hscale=%d",
                 pvrrc.scaler_ctl.vscalefactor, pvrrc.scaler_ctl.vscalefactor, pvrrc.scaler_ctl.hscale);
    }
}

void MetalDrawer::DrawPoly(id<MTLRenderCommandEncoder> encoder, u32 listType, bool sortTriangles, const PolyParam &poly, u32 first, u32 count)
{
    MTLScissorRect scissorRect {};
    TileClipping tileClip = SetTileClip(encoder, poly.tileclip, scissorRect);

    float trilinearAlpha = 1.0f;
    if (poly.tsp.FilterMode > 1 && poly.pcw.Texture && listType != ListType_Punch_Through && poly.tcw.MipMapped == 1)
    {
        trilinearAlpha = 0.25f * (poly.tsp.MipMapD & 0x3);
        if (poly.tsp.FilterMode == 2)
            // Trilinear pass A
            trilinearAlpha = 1.0f - trilinearAlpha;
    }
    int gpuPalette = poly.texture == nullptr || !poly.texture->gpuPalette ? 0
                                                                          : poly.tsp.FilterMode + 1;
    float palette_index = 0.0f;
    if (gpuPalette != 0)
    {
        if (config::TextureFiltering == 1)
            gpuPalette = 1;
        else if (config::TextureFiltering == 2)
            gpuPalette = 2;
        if (poly.tcw.PixelFmt == PixelPal4)
            palette_index = float(poly.tcw.PalSelect << 4) / 1023.0f;
        else
            palette_index = float(poly.tcw.PalSelect >> 4 << 8) / 1023.0f;
    }

    std::array<float, 6> pushConstants;

    if (tileClip == TileClipping::Inside || trilinearAlpha != 1.0f || gpuPalette != 0)
    {
        pushConstants = {
                (float)scissorRect.x,
                (float)scissorRect.y,
                (float)scissorRect.x + (float)scissorRect.width,
                (float)scissorRect.y + (float)scissorRect.height,
                trilinearAlpha,
                palette_index
        };
    } else {
        pushConstants = { 0, 0, 0, 0, 0, 0 };
    }

    [encoder setFragmentBytes:pushConstants.data() length:sizeof(pushConstants) + MetalBufferPacker::align(sizeof(pushConstants), 16) atIndex:1];

    bool shadowed = listType == ListType_Opaque || listType == ListType_Punch_Through;

    auto pipeline = pipelineManager->GetPipeline(listType, sortTriangles, poly, gpuPalette, dithering);
    INFO_LOG(RENDERER, "Using pipeline for poly: pcw.Texture=%d, texture=%p", (int)poly.pcw.Texture, poly.texture);
    [encoder setRenderPipelineState:pipeline];
    [encoder setDepthStencilState:pipelineManager->GetDepthStencilStates(listType, sortTriangles, shadowed, poly)];
    [encoder setCullMode:toMetalCullMode(poly.isp.CullMode)];

    if (shadowed) {
        if (poly.pcw.Shadow != 0) {
            [encoder setStencilReferenceValue:0x80];
        } else {
            [encoder setStencilReferenceValue:0];
        }
    }

    INFO_LOG(RENDERER, "Texture binding check: poly.pcw.Texture=%d, poly.texture=%p", (int)poly.pcw.Texture, poly.texture);

    if (poly.texture != nullptr) {
        auto texture = ((MetalTexture *)poly.texture)->GetReadOnlyTexture();
        [encoder setFragmentTexture:texture atIndex:0];
        INFO_LOG(RENDERER, "Binding actual texture and sampler for poly");

        // Texture sampler
        id<MTLSamplerState> samplerState = samplers->GetSampler(poly, listType == ListType_Punch_Through);
        if (samplerState == nil) {
            ERROR_LOG(RENDERER, "GetSampler returned nil! poly.texture=%p, FilterMode=%d, ClampU=%d, ClampV=%d, listType=%d",
                     poly.texture, (int)poly.tsp.FilterMode, (int)poly.tsp.ClampU, (int)poly.tsp.ClampV, (int)listType);
            // Skip binding nil sampler to prevent objc_msg_send crash
            return;
        }
        [encoder setFragmentSamplerState:samplerState atIndex:0];
    } else if (poly.pcw.Texture) {
        // Shader expects texture parameters but poly.texture is null
        // This can happen in some edge cases - bind default texture and sampler
        // Create a minimal 1x1 white texture as placeholder
        INFO_LOG(RENDERER, "Shader expects texture but poly.texture is null - binding default texture");
        static id<MTLTexture> defaultTexture = nil;
        if (defaultTexture == nil) {
            MTLTextureDescriptor* texDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                                 width:1 height:1 mipmapped:NO];
            defaultTexture = [MetalContext::Instance()->GetDevice() newTextureWithDescriptor:texDesc];
            uint32_t whitePixel = 0xFFFFFFFF;
            [defaultTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&whitePixel bytesPerRow:4];
            INFO_LOG(RENDERER, "Created default 1x1 white texture");
        }
        [encoder setFragmentTexture:defaultTexture atIndex:0];

        TSP defaultTsp = {};
        id<MTLSamplerState> defaultSampler = samplers->GetSampler(defaultTsp);
        if (defaultSampler == nil) {
            ERROR_LOG(RENDERER, "GetSampler returned nil for default TSP!");
            return;
        }
        [encoder setFragmentSamplerState:defaultSampler atIndex:0];
    }
    // Note: When poly.pcw.Texture is false, the shader is compiled without texture/sampler parameters,
    // so we don't bind anything in that case to avoid Metal validation errors

    if (poly.pcw.Texture || poly.isNaomi2())
    {
        u64 offset = 0;
        u32 index = 0;
        if (poly.isNaomi2())
        {
            switch (listType)
            {
                case ListType_Opaque:
                    offset = offsets.naomi2OpaqueOffset;
                    index = &poly - &pvrrc.global_param_op[0];
                    // Bounds check to prevent invalid offsets
                    if (index >= pvrrc.global_param_op.size()) {
                        WARN_LOG(RENDERER, "Invalid Naomi2 opaque poly index: %u >= %zu", index, pvrrc.global_param_op.size());
                        return;
                    }
                    break;
                case ListType_Punch_Through:
                    offset = offsets.naomi2PunchThroughOffset;
                    index = &poly - &pvrrc.global_param_pt[0];
                    // Bounds check to prevent invalid offsets
                    if (index >= pvrrc.global_param_pt.size()) {
                        WARN_LOG(RENDERER, "Invalid Naomi2 punch-through poly index: %u >= %zu", index, pvrrc.global_param_pt.size());
                        return;
                    }
                    break;
                case ListType_Translucent:
                    offset = offsets.naomi2TranslucentOffset;
                    index = &poly - &pvrrc.global_param_tr[0];
                    // Bounds check to prevent invalid offsets
                    if (index >= pvrrc.global_param_tr.size()) {
                        WARN_LOG(RENDERER, "Invalid Naomi2 translucent poly index: %u >= %zu", index, pvrrc.global_param_tr.size());
                        return;
                    }
                    break;
            }
        }

        size_t size = sizeof(MetalN2VertexShaderUniforms) + MetalBufferPacker::align(sizeof(MetalN2VertexShaderUniforms), 16);
        u64 finalOffset = offset + index * size;
        // Bounds check for final offset to prevent buffer overflow
        if (finalOffset >= [curMainBuffer length]) {
            WARN_LOG(RENDERER, "Buffer offset out of bounds: %llu >= %lu", finalOffset, (unsigned long)[curMainBuffer length]);
            return;
        }
        [encoder setVertexBuffer:curMainBuffer offset:finalOffset atIndex:1];

        if (offsets.lightsOffset != -1) {
            size = sizeof(N2LightModel) + MetalBufferPacker::align(sizeof(N2LightModel), 16);
            // Bounds check for lightModel to prevent buffer overflow
            if (poly.lightModel >= 256) {  // Reasonable upper bound for light models
                WARN_LOG(RENDERER, "Invalid light model index: %u", poly.lightModel);
                return;
            }
            u64 lightsOffset = offsets.lightsOffset + poly.lightModel * size;
            // Bounds check for final lights offset
            if (lightsOffset >= [curMainBuffer length]) {
                WARN_LOG(RENDERER, "Lights buffer offset out of bounds: %llu >= %lu", lightsOffset, (unsigned long)[curMainBuffer length]);
                return;
            }
            [encoder setVertexBuffer:curMainBuffer offset:lightsOffset atIndex:2];
        }
    }

    MTLPrimitiveType primitive = sortTriangles && !config::PerStripSorting ? MTLPrimitiveTypeTriangle : MTLPrimitiveTypeTriangleStrip;

    [encoder drawIndexedPrimitives:primitive
                        indexCount:count
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:curMainBuffer
                 indexBufferOffset:offsets.indexOffset + first * sizeof(u32)];
}

void MetalDrawer::DrawSorted(id<MTLRenderCommandEncoder> encoder, const std::vector<SortedTriangle> &polys, u32 first, u32 last, bool multipass)
{
    if (first == last)
        return;

    [encoder pushDebugGroup:@"DrawSorted"];

    for (u32 idx = first; idx < last; idx++)
        DrawPoly(encoder, ListType_Translucent, true, pvrrc.global_param_tr[polys[idx].polyIndex], polys[idx].first, polys[idx].count);
    if (multipass && config::TranslucentPolygonDepthMask)
    {
        // Write to the depth buffer now. The next render pass might need it. (Cosmic Smash)
        for (u32 idx = first; idx < last; idx++)
        {
            const SortedTriangle& param = polys[idx];
            const PolyParam& polyParam = pvrrc.global_param_tr[param.polyIndex];
            if (polyParam.isp.ZWriteDis)
                continue;
            [encoder setRenderPipelineState:pipelineManager->GetDepthPassPipeline(polyParam.isNaomi2())];
            [encoder setDepthStencilState:pipelineManager->GetDepthPassDepthStencilStates(polyParam.isNaomi2())];

            MTLScissorRect scissorRect {};
            SetTileClip(encoder, polyParam.tileclip, scissorRect);

            [encoder setCullMode:toMetalCullMode(polyParam.isp.CullMode)];

            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:param.count
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:curMainBuffer
                         indexBufferOffset:offsets.indexOffset + param.first * sizeof(u32)];
        }
    }

    [encoder popDebugGroup];
}

void MetalDrawer::DrawList(id<MTLRenderCommandEncoder> encoder, u32 listType, bool sortTriangles, const std::vector<PolyParam> &polys, u32 first, u32 last)
{
    if (first == last)
        return;

    [encoder pushDebugGroup:@"DrawList"];

    const PolyParam *pp_end = polys.data() + last;
    for (const PolyParam *pp = &polys[first]; pp != pp_end; pp++)
        if (pp->count > 2)
            DrawPoly(encoder, listType, sortTriangles, *pp, pp->first, pp->count);

    [encoder popDebugGroup];
}

void MetalDrawer::DrawModVols(id<MTLRenderCommandEncoder> encoder, int first, int count)
{
    if (count == 0 || pvrrc.modtrig.empty() || !config::ModifierVolumes)
        return;

    [encoder pushDebugGroup:@"DrawModVols"];
    [encoder setVertexBufferOffset:offsets.modVolOffset atIndex:30];

    ModifierVolumeParam* params = &pvrrc.global_param_mvo[first];

    int mod_base = -1;
    id<MTLRenderPipelineState> state;
    id<MTLDepthStencilState> depth_state;

    const std::array<float, 1> pushConstants = { 1 - FPU_SHAD_SCALE.scale_factor / 256.f };
    [encoder setFragmentBytes:pushConstants.data() length:sizeof(pushConstants) + MetalBufferPacker::align(sizeof(pushConstants), 16) atIndex:1];

    for (int cmv = 0; cmv < count; cmv++) {
        ModifierVolumeParam& param = params[cmv];

        if (param.count == 0)
            continue;

        u32 mv_mode = param.isp.DepthMode;

        if (mod_base == -1)
            mod_base = param.first;

        if (!param.isp.VolumeLast && mv_mode > 0) {
            state = pipelineManager->GetModifierVolumePipeline(ModVolMode::Or, param.isNaomi2());  // OR'ing (open volume or quad)
            depth_state = pipelineManager->GetModVolDepthStencilStates(ModVolMode::Or, param.isNaomi2());
        } else {
            state = pipelineManager->GetModifierVolumePipeline(ModVolMode::Xor, param.isNaomi2()); // XOR'ing (closed volume)
            depth_state = pipelineManager->GetModVolDepthStencilStates(ModVolMode::Xor, param.isNaomi2());
        }

        [encoder setRenderPipelineState:state];
        [encoder setDepthStencilState:depth_state];
        [encoder setCullMode:toMetalCullMode(param.isp.CullMode)];
        [encoder setStencilReferenceValue:2];
        MTLScissorRect scissorRect {};
        SetTileClip(encoder, param.tileclip, scissorRect);
        // TODO inside clipping

        [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:param.first * 3
                    vertexCount:param.count * 3];

        if (mv_mode == 1 || mv_mode == 2)
        {
            // Sum the area
            state = pipelineManager->GetModifierVolumePipeline(mv_mode == 1 ? ModVolMode::Inclusion : ModVolMode::Exclusion, param.isNaomi2());
            depth_state = pipelineManager->GetModVolDepthStencilStates(mv_mode == 1 ? ModVolMode::Inclusion : ModVolMode::Exclusion, param.isNaomi2());
            [encoder setRenderPipelineState:state];
            [encoder setDepthStencilState:depth_state];
            [encoder setCullMode:toMetalCullMode(param.isp.CullMode)];
            [encoder setStencilReferenceValue:1];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart: mod_base * 3
                        vertexCount: (param.first + param.count - mod_base) * 3];
            mod_base = -1;
        }
    }
    [encoder setVertexBufferOffset:0 atIndex:30];

    state = pipelineManager->GetModifierVolumePipeline(ModVolMode::Final, false);
    depth_state = pipelineManager->GetModVolDepthStencilStates(ModVolMode::Final, false);
    [encoder setRenderPipelineState:state];
    [encoder setDepthStencilState:depth_state];
    [encoder setCullMode:toMetalCullMode(0)];
    [encoder setStencilReferenceValue:0x81];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                        indexCount:4
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:curMainBuffer
                 indexBufferOffset:offsets.indexOffset];

    [encoder popDebugGroup];
}

void MetalDrawer::UploadMainBuffer(const MetalVertexShaderUniforms &vertexUniforms, const MetalFragmentShaderUniforms &fragmentUniforms) {
    MetalBufferPacker packer;

    // Vertex
    packer.add(pvrrc.verts.data(), pvrrc.verts.size() * sizeof(decltype(*pvrrc.verts.data())));
    // Modifier Volumes
    offsets.modVolOffset = packer.add(pvrrc.modtrig.data(), pvrrc.modtrig.size() * sizeof(decltype(*pvrrc.modtrig.data())));
    // Index
    offsets.indexOffset = packer.add(pvrrc.idx.data(), pvrrc.idx.size() * sizeof(decltype(*pvrrc.idx.data())));
    // Uniform buffers
    offsets.vertexUniformOffset = packer.addUniform(&vertexUniforms, sizeof(vertexUniforms));
    offsets.fragmentUniformOffset = packer.addUniform(&fragmentUniforms, sizeof(fragmentUniforms));

    std::vector<u8> n2uniforms;
    if (settings.platform.isNaomi2())
    {
         packNaomi2Uniforms(packer, offsets, n2uniforms, false);
         offsets.lightsOffset = packNaomi2Lights(packer);
    }

    MetalBufferData *buffer = new MetalBufferData(packer.size());
    packer.upload(*buffer);
    curMainBuffer = buffer->buffer;
}

bool MetalDrawer::Draw(const MetalTexture *fogTexture, const MetalTexture *paletteTexture) {
    MetalFragmentShaderUniforms fragUniforms = MakeFragmentUniforms<MetalFragmentShaderUniforms>();
    dithering = config::EmulateFramebuffer && pvrrc.fb_W_CTRL.fb_dither && pvrrc.fb_W_CTRL.fb_packmode <= 3;
    if (dithering) {
        switch (pvrrc.fb_W_CTRL.fb_packmode)
        {
            case 0: // 0555 KRGB 16 bit
            case 3: // 1555 ARGB 16 bit
                fragUniforms.ditherDivisor[0] = fragUniforms.ditherDivisor[1] = fragUniforms.ditherDivisor[2] = 2.f;
                break;
            case 1: // 565 RGB 16 bit
                fragUniforms.ditherDivisor[0] = fragUniforms.ditherDivisor[2] = 2.f;
                fragUniforms.ditherDivisor[1] = 4.f;
                break;
            case 2: // 4444 ARGB 16 bit
                fragUniforms.ditherDivisor[0] = fragUniforms.ditherDivisor[1] = fragUniforms.ditherDivisor[2] = 1.f;
                break;
            default:
                break;
        }
        fragUniforms.ditherDivisor[3] = 1.f;
    }

    currentScissor = MTLScissorRect {};

    @autoreleasepool {
        id<MTLRenderCommandEncoder> renderEncoder = BeginRenderPass();
        [renderEncoder retain];

        [renderEncoder setFragmentTexture:fogTexture->GetTexture() atIndex:2];
        [renderEncoder setFragmentTexture:paletteTexture->GetTexture() atIndex:3];

        // Fog sampler
        TSP fogTsp = {};
        fogTsp.FilterMode = 1;
        fogTsp.ClampU = 1;
        fogTsp.ClampV = 1;
        id<MTLSamplerState> fogSampler = samplers->GetSampler(fogTsp);
        if (fogSampler == nil) {
            ERROR_LOG(RENDERER, "GetSampler returned nil for fog TSP!");
            return false;
        }
        [renderEncoder setFragmentSamplerState:fogSampler atIndex:2];

        // Palette sampler
        TSP palTsp = {};
        palTsp.FilterMode = 0;
        palTsp.ClampU = 1;
        palTsp.ClampV = 1;
        id<MTLSamplerState> paletteSampler = samplers->GetSampler(palTsp);
        if (paletteSampler == nil) {
            ERROR_LOG(RENDERER, "GetSampler returned nil for palette TSP!");
            return false;
        }
        [renderEncoder setFragmentSamplerState:paletteSampler atIndex:3];

        setFirstProvokingVertex(pvrrc);

        // Upload vertex and index buffers
        MetalVertexShaderUniforms vtxUniforms {};
        vtxUniforms.ndcMat = matrices.GetNormalMatrix();

        [renderEncoder setFrontFacingWinding:MTLWindingCounterClockwise];

        UploadMainBuffer(vtxUniforms, fragUniforms);

        [renderEncoder setVertexBuffer:curMainBuffer offset:0 atIndex:30];
        [renderEncoder setVertexBuffer:curMainBuffer offset:offsets.vertexUniformOffset atIndex:0];
        [renderEncoder setFragmentBuffer:curMainBuffer offset:offsets.fragmentUniformOffset atIndex:0];

        RenderPass previous_pass {};
        for (int render_pass = 0; render_pass < (int)pvrrc.render_passes.size(); render_pass++) {
            const RenderPass& current_pass = pvrrc.render_passes[render_pass];

            DEBUG_LOG(RENDERER, "Render pass %d OP %d PT %d TR %d MV %d autosort %d", render_pass + 1,
                      current_pass.op_count - previous_pass.op_count,
                      current_pass.pt_count - previous_pass.pt_count,
                      current_pass.tr_count - previous_pass.tr_count,
                      current_pass.mvo_count - previous_pass.mvo_count, current_pass.autosort);
            DrawList(renderEncoder, ListType_Opaque, false, pvrrc.global_param_op, previous_pass.op_count, current_pass.op_count);
            DrawList(renderEncoder, ListType_Punch_Through, false, pvrrc.global_param_pt, previous_pass.pt_count, current_pass.pt_count);
            DrawModVols(renderEncoder, previous_pass.mvo_count, current_pass.mvo_count - previous_pass.mvo_count);
            if (current_pass.autosort) {
                if (!config::PerStripSorting)
                    DrawSorted(renderEncoder, pvrrc.sortedTriangles, previous_pass.sorted_tr_count, current_pass.sorted_tr_count, render_pass + 1 < (int)pvrrc.render_passes.size());
                else
                    DrawList(renderEncoder, ListType_Translucent, true, pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count);
            } else {
                DrawList(renderEncoder, ListType_Translucent, false, pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count);
            }
            previous_pass = current_pass;
        }
    }

    curMainBuffer = nil;

    return !pvrrc.isRTT;
}

void MetalTextureDrawer::Init(MetalSamplers *samplers, MetalShaders *shaders, MetalTextureCache *textureCache)
{
    MetalDrawer::Init(samplers, MetalPipelineManager(shaders));

    this->textureCache = textureCache;

    rttPassDescriptor = [[MTLRenderPassDescriptor alloc] init];
}

id<MTLRenderCommandEncoder> MetalTextureDrawer::BeginRenderPass() {
    INFO_LOG(RENDERER, "RenderToTexture packmode=%d stride=%d - %d x %d @ %06x", pvrrc.fb_W_CTRL.fb_packmode, pvrrc.fb_W_LINESTRIDE * 8,
              pvrrc.fb_X_CLIP.max + 1, pvrrc.fb_Y_CLIP.max + 1, pvrrc.fb_W_SOF1 & VRAM_MASK);
    matrices.CalcMatrices(&pvrrc);

    textureAddr = pvrrc.fb_W_SOF1 & VRAM_MASK;
    u32 origWidth = pvrrc.getFramebufferWidth();
    u32 origHeight = pvrrc.getFramebufferHeight();
    u32 upscaledWidth = origWidth;
    u32 upscaledHeight = origHeight;
    u32 widthPow2;
    u32 heightPow2;
    getRenderToTextureDimensions(upscaledWidth, upscaledHeight, widthPow2, heightPow2);

    INFO_LOG(RENDERER, "RTT Dimensions: orig=(%dx%d) -> upscaled=(%dx%d) -> pow2=(%dx%d)",
              origWidth, origHeight, upscaledWidth, upscaledHeight, widthPow2, heightPow2);

    // Validate dimensions to prevent crashes with extreme values
    if (widthPow2 == 0 || heightPow2 == 0 || widthPow2 > 16384 || heightPow2 > 16384) {
        ERROR_LOG(RENDERER, "Invalid RTT dimensions: pow2=%dx%d, falling back to safe values", widthPow2, heightPow2);
        widthPow2 = std::max(8u, std::min(upscaledWidth, 1024u));
        heightPow2 = std::max(8u, std::min(upscaledHeight, 1024u));
        // Ensure power of 2
        widthPow2 = 1 << (int)ceil(log2(widthPow2));
        heightPow2 = 1 << (int)ceil(log2(heightPow2));
        INFO_LOG(RENDERER, "RTT Fallback dimensions: pow2=(%dx%d)", widthPow2, heightPow2);
    }

    id<MTLCommandBuffer> commandBuffer = commandPool->Allocate();

    if (!depthAttachment || widthPow2 > depthAttachment.width || heightPow2 > depthAttachment.height)
    {
        INFO_LOG(RENDERER, "Creating new depth attachment: %dx%d", widthPow2, heightPow2);
        MTLTextureDescriptor *depthDescriptor = [[MTLTextureDescriptor alloc] init];
        depthDescriptor.width = widthPow2;
        depthDescriptor.height = heightPow2;
        depthDescriptor.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        depthDescriptor.usage = MTLTextureUsageRenderTarget;
        depthDescriptor.storageMode = MTLStorageModePrivate;

        depthAttachment = [MetalContext::Instance()->GetDevice() newTextureWithDescriptor:depthDescriptor];
        [depthAttachment setLabel:@"Rtt Depth Attachment"];
    } else {
        INFO_LOG(RENDERER, "Reusing existing depth attachment: %dx%d", (int)depthAttachment.width, (int)depthAttachment.height);
    }

    id<MTLTexture> colorImage;

    if (config::RenderToTextureBuffer)
    {
        texture = textureCache->getRTTexture(textureAddr, pvrrc.fb_W_CTRL.fb_packmode, origWidth, origHeight);
        if (textureCache->IsInFlight(texture, false))
        {
            texture->CreateReadOnlyCopy(commandBuffer);
            texture->deferDeleteResource(commandPool);
        }
        textureCache->SetInFlight(texture);

        // Check if we need to recreate the texture
        bool needsRecreation = !texture->GetTexture() ||
                               texture->GetTexture().width != widthPow2 ||
                               texture->GetTexture().height != heightPow2;

        if (needsRecreation)
        {
            INFO_LOG(RENDERER, "Creating RTT texture: %dx%d", widthPow2, heightPow2);
            MTLTextureDescriptor *colorDescriptor = [[MTLTextureDescriptor alloc] init];
            colorDescriptor.width = widthPow2;
            colorDescriptor.height = heightPow2;
            colorDescriptor.pixelFormat = MTLPixelFormatBGRA8Unorm;
            colorDescriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            colorDescriptor.storageMode = MTLStorageModePrivate;

            id<MTLTexture> newTexture = [MetalContext::Instance()->GetDevice() newTextureWithDescriptor:colorDescriptor];
            [newTexture setLabel:@"Rtt Color Attachment"];
            texture->SetTexture(newTexture, widthPow2, heightPow2);
        } else {
            INFO_LOG(RENDERER, "Reusing existing RTT texture: %dx%d", (int)texture->GetTexture().width, (int)texture->GetTexture().height);
        }
        colorImage = texture->GetTexture();
    }
    else
    {
        if (!colorAttachment || widthPow2 > colorAttachment.width || heightPow2 > colorAttachment.height)
        {
            INFO_LOG(RENDERER, "Creating new color attachment: %dx%d", widthPow2, heightPow2);
            MTLTextureDescriptor *colorDescriptor = [[MTLTextureDescriptor alloc] init];
            colorDescriptor.width = widthPow2;
            colorDescriptor.height = heightPow2;
            colorDescriptor.pixelFormat = MTLPixelFormatBGRA8Unorm;
            colorDescriptor.usage = MTLTextureUsageRenderTarget;
            colorDescriptor.storageMode = MTLStorageModePrivate;

            colorAttachment = [MetalContext::Instance()->GetDevice() newTextureWithDescriptor:colorDescriptor];
            [colorAttachment setLabel:@"Rtt Color Attachment"];
        } else {
            INFO_LOG(RENDERER, "Reusing existing color attachment: %dx%d", (int)colorAttachment.width, (int)colorAttachment.height);
        }
        colorImage = colorAttachment;
    }

    auto colorAttachmentDesc = rttPassDescriptor.colorAttachments[0];
    [colorAttachmentDesc setTexture:colorImage];
    [colorAttachmentDesc setLoadAction:MTLLoadActionClear];
    [colorAttachmentDesc setStoreAction:MTLStoreActionStore];
    [colorAttachmentDesc setClearColor:MTLClearColorMake(0.0, 0.0, 0.0, 1.0)];

    auto depthAttachmentDesc = rttPassDescriptor.depthAttachment;
    [depthAttachmentDesc setTexture:depthAttachment];
    [depthAttachmentDesc setLoadAction:MTLLoadActionClear];
    [depthAttachmentDesc setStoreAction:MTLStoreActionDontCare];
    [depthAttachmentDesc setClearDepth:0.0];

    auto stencilAttachmentDesc = rttPassDescriptor.stencilAttachment;
    [stencilAttachmentDesc setTexture:depthAttachment];
    [stencilAttachmentDesc setLoadAction:MTLLoadActionClear];
    [stencilAttachmentDesc setStoreAction:MTLStoreActionDontCare];
    [stencilAttachmentDesc setClearStencil:0];

    currentEncoder = [commandBuffer renderCommandEncoderWithDescriptor:rttPassDescriptor];
    [currentEncoder pushDebugGroup:@"RenderToTexture"];

    // Log actual render target dimensions from the encoder
    INFO_LOG(RENDERER, "ACTUAL render target dimensions: color=%dx%d, depth=%dx%d",
              (int)colorImage.width, (int)colorImage.height,
              (int)depthAttachment.width, (int)depthAttachment.height);

    MTLViewport viewport = {
            0.0,
            0.0,
            (double)upscaledWidth,
            (double)upscaledHeight,
            1.0,
            0.0
    };
    [currentEncoder setViewport:viewport];
    INFO_LOG(RENDERER, "RTT Viewport: %.0fx%.0f", viewport.width, viewport.height);

    // Fix coordinate calculation - use proper coordinate function
    u32 minX = pvrrc.getFramebufferMinX() * upscaledWidth / origWidth;
    u32 minY = pvrrc.getFramebufferMinY() * upscaledHeight / origHeight;

    // Use the coordinate function to ensure bounds safety
    getRenderToTextureCoordinates(minX, minY, widthPow2, heightPow2);

    // Calculate scissor dimensions safely
    u32 scissorWidth = std::min(upscaledWidth, widthPow2 - minX);
    u32 scissorHeight = std::min(upscaledHeight, heightPow2 - minY);

    // Ensure minimum scissor size
    scissorWidth = std::max(scissorWidth, 1u);
    scissorHeight = std::max(scissorHeight, 1u);

    INFO_LOG(RENDERER, "RTT Scissor calculation: orig=%dx%d, upscaled=%dx%d, pow2=%dx%d, min=(%d,%d), scissor=(%d,%d,%d,%d)",
             origWidth, origHeight, upscaledWidth, upscaledHeight, widthPow2, heightPow2,
             minX, minY, minX, minY, scissorWidth, scissorHeight);

    // Set baseScissor to the ACTUAL render target dimensions for bounds checking
    baseScissor = MTLScissorRect { 0, 0, (NSUInteger)colorImage.width, (NSUInteger)colorImage.height };
    INFO_LOG(RENDERER, "RTT baseScissor set to: (%d,%d,%d,%d)",
              (int)baseScissor.x, (int)baseScissor.y, (int)baseScissor.width, (int)baseScissor.height);

    // Now set the actual scissor rect using SetScissor to ensure bounds checking
    MTLScissorRect requestedScissor = MTLScissorRect { minX, minY, scissorWidth, scissorHeight };
    INFO_LOG(RENDERER, "RTT Requesting scissor: (%d,%d,%d,%d)",
              (int)requestedScissor.x, (int)requestedScissor.y, (int)requestedScissor.width, (int)requestedScissor.height);

    SetScissor(currentEncoder, requestedScissor);

    return currentEncoder;
}

void MetalTextureDrawer::EndRenderPass()
{
    [currentEncoder popDebugGroup];
    [currentEncoder endEncoding];
    currentEncoder = nil;

    u32 clippedWidth = pvrrc.getFramebufferWidth();
    u32 clippedHeight = pvrrc.getFramebufferHeight();

    if (config::RenderToTextureBuffer)
    {
        commandPool->EndFrameAndWait();

        u16 *dst = (u16 *)&vram[textureAddr];

        PixelBuffer<u32> tmpBuf;
        tmpBuf.init(clippedWidth, clippedHeight);
        // TODO: WRITE TO BUFFER
        WriteTextureToVRam(clippedWidth, clippedHeight, (u8 *)tmpBuf.data(), dst, pvrrc.fb_W_CTRL, pvrrc.fb_W_LINESTRIDE * 8);
    }
    else
    {
        commandPool->EndFrame();
        texture->dirty = 0;
        texture->unprotectVRam();
    }

    MetalDrawer::EndRenderPass();
}

void MetalScreenDrawer::Init(MetalSamplers *samplers, MetalShaders *shaders, const MTLViewport &viewport) {
    emulateFramebuffer = config::EmulateFramebuffer;
    this->shaderManager = shaders;

    // Validate viewport dimensions
    if (viewport.width <= 0 || viewport.height <= 0 || viewport.width > 16384 || viewport.height > 16384) {
        WARN_LOG(RENDERER, "Invalid viewport dimensions: %.0fx%.0f, skipping initialization", viewport.width, viewport.height);
        return;
    }

    bool dimensionsChanged = this->viewport.height != viewport.height ||
            this->viewport.width != viewport.width ||
            this->viewport.originX != viewport.originX ||
            this->viewport.originY != viewport.originY ||
            this->viewport.zfar != viewport.zfar ||
            this->viewport.znear != viewport.znear;

    if (dimensionsChanged) {
        DEBUG_LOG(RENDERER, "MetalScreenDrawer dimension change: %.0fx%.0f -> %.0fx%.0f",
                 this->viewport.width, this->viewport.height, viewport.width, viewport.height);

        if (!framebuffers.empty()) {
            verify(commandPool != nullptr);
            commandPool->addToFlight(new MetalDeleter(std::move(framebuffers)));
        }
        if (depthAttachment) {
            class ResourceDeleter : public MetalDeletable
            {
            public:
                ResourceDeleter(id<MTLTexture> texture)
                {
                    std::swap(this->texture, texture);
                }

                ~ResourceDeleter() override {
                    [texture setPurgeableState:MTLPurgeableStateEmpty];
                    texture = nil;
                }

            private:
                id<MTLTexture> texture = nil;
            };

            commandPool->addToFlight(new ResourceDeleter(depthAttachment));
        }

        depthAttachment = nil;
        clearPassDescriptors.clear();
        loadPassDescriptors.clear();
        clearNeeded.clear();
    }
    this->viewport = viewport;

    if (depthAttachment == nil)
    {
        MTLTextureDescriptor *descriptor = [[MTLTextureDescriptor alloc] init];
        descriptor.width = (NSUInteger)viewport.width;
        descriptor.height = (NSUInteger)viewport.height;
        descriptor.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;

        depthAttachment = [MetalContext::Instance()->GetDevice() newTextureWithDescriptor:descriptor];
        [depthAttachment setLabel:@"Screen Depth Attachment"];
    }

    if (framebuffers.size() > 3)
    {
        framebuffers.resize(3);
        loadPassDescriptors.resize(3);
        clearPassDescriptors.resize(3);
        clearNeeded.resize(3);
    }
    else
    {
        while (framebuffers.size() < 3)
        {
            MTLTextureDescriptor *texDescriptor = [[MTLTextureDescriptor alloc] init];
            texDescriptor.width = viewport.width;
            texDescriptor.height = viewport.height;
            texDescriptor.pixelFormat = MTLPixelFormatBGRA8Unorm;
            texDescriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;

            id<MTLTexture> colorAttachment = [MetalContext::Instance()->GetDevice() newTextureWithDescriptor:texDescriptor];
            framebuffers.push_back(colorAttachment);

            MTLRenderPassDescriptor *passDescriptor = [[MTLRenderPassDescriptor alloc] init];
            auto depth = passDescriptor.depthAttachment;
            [depth setTexture:depthAttachment];
            [depth setLoadAction:MTLLoadActionClear];
            [depth setStoreAction:MTLStoreActionDontCare];

            auto stencil = passDescriptor.stencilAttachment;
            [stencil setTexture:depthAttachment];
            [stencil setLoadAction:MTLLoadActionClear];
            [stencil setStoreAction:MTLStoreActionDontCare];

            auto color = passDescriptor.colorAttachments[0];
            [color setTexture:colorAttachment];
            [color setLoadAction:MTLLoadActionLoad];
            [color setStoreAction:MTLStoreActionStore];

            loadPassDescriptors.push_back(passDescriptor);

            MTLRenderPassDescriptor *clearPassDescriptor = [passDescriptor copy];
            [clearPassDescriptor.colorAttachments[0] setLoadAction:MTLLoadActionClear];

            clearPassDescriptors.push_back(clearPassDescriptor);
            clearNeeded.push_back(true);
        }
    }
    frameRendered = false;

    MetalDrawer::Init(samplers, MetalPipelineManager(shaderManager));
}

id<MTLRenderCommandEncoder> MetalScreenDrawer::BeginRenderPass() {
    INFO_LOG(RENDERER, "=== SCREEN DRAWER BeginRenderPass ===");

    if (!renderPassStarted)
    {
        frameRendered = false;
        id<MTLCommandBuffer> commandBuffer = commandPool->Allocate();
        MTLRenderPassDescriptor* passDescriptor = clearNeeded[GetCurrentImage()] || pvrrc.clearFramebuffer ? clearPassDescriptors[GetCurrentImage()] : loadPassDescriptors[GetCurrentImage()];
        clearNeeded[GetCurrentImage()] = false;
        currentEncoder = [commandBuffer renderCommandEncoderWithDescriptor:passDescriptor];
        renderPassStarted = true;

        INFO_LOG(RENDERER, "Screen render pass started, imageIndex=%d", GetCurrentImage());
    }

    [currentEncoder setViewport:viewport];
    INFO_LOG(RENDERER, "Screen viewport set: %.0fx%.0f at (%.0f,%.0f)", viewport.width, viewport.height, viewport.originX, viewport.originY);

    matrices.CalcMatrices(&pvrrc, viewport.width, viewport.height);

    SetBaseScissor(viewport);
    INFO_LOG(RENDERER, "Screen baseScissor calculated: (%d,%d,%d,%d)",
             (int)baseScissor.x, (int)baseScissor.y, (int)baseScissor.width, (int)baseScissor.height);

    INFO_LOG(RENDERER, "About to call SetScissor with baseScissor");
    SetScissor(currentEncoder, baseScissor);
    INFO_LOG(RENDERER, "SetScissor completed");

    return currentEncoder;
}

void MetalScreenDrawer::EndRenderPass() {
    if (!renderPassStarted)
        return;

    [currentEncoder endEncoding];
    currentEncoder = nil;

    if (emulateFramebuffer)
    {
        // TODO: scaleAndWriteFramebuffer
    }
    else
    {

        aspectRatio = getOutputFramebufferAspectRatio();
    }
    commandPool->EndFrame();
    MetalDrawer::EndRenderPass();
    frameRendered = true;
}
