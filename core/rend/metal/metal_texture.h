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

#pragma once
#include "rend/TexCache.h"
#include "metal_context.h"
#include "metal.h"

#include <unordered_set>
#include <Metal/Metal.h>

class MetalTexture final : public BaseTextureCacheData
{
public:
    MetalTexture(TSP tsp = {}, TCW tcw = {}) : BaseTextureCacheData(tsp, tcw) {}

    std::string GetId() override {
        if (@available(iOS 16.0, macOS 13.0, *)) {
            return std::to_string([texture gpuResourceID]._impl);
        } else {
            // Fallback for older iOS versions - use pointer address
            return std::to_string(reinterpret_cast<uintptr_t>(texture));
        }
    }
    id<MTLTexture> GetTexture() const { return texture; }
    void UploadToGPU(int width, int height, const u8 *data, bool mipmapped, bool mipmapsIncluded = false) override;
    void SetCommandBuffer(id<MTLCommandBuffer> commandBuffer) { this->commandBuffer = commandBuffer; }
    void SetTexture(id<MTLTexture> texture, u32 width, u32 height) {
        this->texture = texture;
        this->width = width;
        this->height = height;
    }
    void SetInFlight(bool inFlight) {
        this->isInFlight = inFlight;
    }
    void deferDeleteResource(MetalFlightManager *manager);
    id<MTLTexture> GetReadOnlyTexture() const { return readOnlyTexture ? readOnlyTexture : texture; }
    void CreateReadOnlyCopy(id<MTLCommandBuffer> commandBuffer);

private:
    void Init(u32 width, u32 height, MTLPixelFormat format, u32 dataSize, bool mipmapped, bool mipmapsIncluded);
    void SetImage(u32 srcSize, const void *srcData, bool genMipmaps);
    void GenerateMipmaps();

    MTLPixelFormat format = MTLPixelFormatInvalid;
    u32 width = 0;
    u32 height = 0;
    u32 mipmapLevels = 1;
    id<MTLCommandBuffer> commandBuffer = nil;
    id<MTLTexture> texture = nil;
    id<MTLTexture> readOnlyTexture = nil;
    bool isInFlight = false;

    friend class MetalTextureCache;
};

class MetalSamplers
{
public:
    explicit MetalSamplers();
    ~MetalSamplers();

    static const u32 TSP_Mask = 0x7ef00;

    void term() {
        samplers.clear();
    }

    id<MTLSamplerState> GetSampler(const PolyParam& poly, bool punchThrough, bool texture1 = false) {
        TSP tsp = texture1 ? poly.tsp1 : poly.tsp;
        if (poly.texture != nullptr && poly.texture->gpuPalette)
            tsp.FilterMode = 0;
        else if (config::TextureFiltering == 1)
            tsp.FilterMode = 0;
        else if (config::TextureFiltering == 2)
            tsp.FilterMode = 1;
        return GetSampler(tsp, punchThrough);
    }

    id<MTLSamplerState> GetSampler(TSP tsp, bool punchThrough = false) {
        const u32 hash = (tsp.full & TSP_Mask) | punchThrough;	// MipMapD, FilterMode, ClampU, ClampV, FlipU, FlipV
        id<MTLSamplerState> sampler = samplers[hash];

        if (!sampler) {
            auto desc = [[MTLSamplerDescriptor alloc] init];

            if (tsp.FilterMode != 0) {
                if (punchThrough) {
                    [desc setMinFilter:MTLSamplerMinMagFilterLinear];
                    [desc setMagFilter:MTLSamplerMinMagFilterLinear];
                    [desc setMipFilter:MTLSamplerMipFilterNearest];
                } else {
                    [desc setMinFilter:MTLSamplerMinMagFilterLinear];
                    [desc setMagFilter:MTLSamplerMinMagFilterLinear];
                    [desc setMipFilter:MTLSamplerMipFilterLinear];
                }
            }
            else {
                [desc setMinFilter:MTLSamplerMinMagFilterNearest];
                [desc setMagFilter:MTLSamplerMinMagFilterNearest];
                [desc setMipFilter:MTLSamplerMipFilterNearest];
            }

            auto sRepeat = tsp.ClampU ? MTLSamplerAddressModeClampToEdge : tsp.FlipU ? MTLSamplerAddressModeMirrorRepeat : MTLSamplerAddressModeRepeat;
            auto tRepeat = tsp.ClampV ? MTLSamplerAddressModeClampToEdge : tsp.FlipV ? MTLSamplerAddressModeMirrorRepeat : MTLSamplerAddressModeRepeat;

            INFO_LOG(RENDERER, "Sampler settings: FlipU=%d, FlipV=%d, sRepeat=%d, tRepeat=%d",
                     (int)tsp.FlipU, (int)tsp.FlipV, (int)sRepeat, (int)tRepeat);

            [desc setSAddressMode:sRepeat];
            [desc setTAddressMode:tRepeat];
            [desc setRAddressMode:tRepeat];
            // Note: Don't set compareFunction for regular color textures
            if (tsp.FilterMode == 1 && !punchThrough) {
                u32 anisotropy = config::AnisotropicFiltering;
                // Clamp anisotropy to valid range (1-16)
                anisotropy = std::max(1u, std::min(16u, anisotropy));
                [desc setMaxAnisotropy:anisotropy];
            } else {
                [desc setMaxAnisotropy:1];
            }

            auto device = MetalContext::Instance()->GetDevice();
            INFO_LOG(RENDERER, "Creating sampler with device=%p, FilterMode=%d, ClampU=%d, ClampV=%d",
                     device, (int)tsp.FilterMode, (int)tsp.ClampU, (int)tsp.ClampV);

                        sampler = [device newSamplerStateWithDescriptor:desc];

            if (sampler == nil) {
                ERROR_LOG(RENDERER, "Failed to create Metal sampler state! Device=%p, FilterMode=%d, ClampU=%d, ClampV=%d, hash=0x%08x",
                         device, (int)tsp.FilterMode, (int)tsp.ClampU, (int)tsp.ClampV, hash);

                // Try to create a basic fallback sampler with minimal settings
                INFO_LOG(RENDERER, "Attempting fallback sampler creation...");
                if (device != nil) {
                    auto fallbackDesc = [[MTLSamplerDescriptor alloc] init];
                    [fallbackDesc setMinFilter:MTLSamplerMinMagFilterNearest];
                    [fallbackDesc setMagFilter:MTLSamplerMinMagFilterNearest];
                    [fallbackDesc setMipFilter:MTLSamplerMipFilterNearest];
                    [fallbackDesc setSAddressMode:MTLSamplerAddressModeClampToEdge];
                    [fallbackDesc setTAddressMode:MTLSamplerAddressModeClampToEdge];
                    [fallbackDesc setRAddressMode:MTLSamplerAddressModeClampToEdge];
                    [fallbackDesc setMaxAnisotropy:1];

                    INFO_LOG(RENDERER, "Creating fallback sampler with device=%p", device);
                    sampler = [device newSamplerStateWithDescriptor:fallbackDesc];
                    if (sampler != nil) {
                        ERROR_LOG(RENDERER, "Created fallback sampler successfully!");
                    } else {
                        ERROR_LOG(RENDERER, "Even fallback sampler creation failed!");
                    }
                } else {
                    ERROR_LOG(RENDERER, "Device is nil, cannot create fallback sampler");
                }
            }

            // Only cache and return if sampler is valid
            if (sampler != nil) {
                samplers[hash] = sampler; // Overwrite any previous (possibly nil) entry
                return sampler;
            } else {
                ERROR_LOG(RENDERER, "No valid sampler created, returning nil");
                return nil;
            }
        }

        return sampler;
    }

private:
    std::unordered_map<u32, id<MTLSamplerState>> samplers;
};

class MetalTextureCache final : public BaseTextureCache<MetalTexture>
{
public:
    MetalTextureCache() {}

    void SetCurrentIndex(int index)
    {
        if (index == (int)currentIndex)
            return;
        if (currentIndex < inFlightTextures.size())
            std::for_each(inFlightTextures[currentIndex].begin(), inFlightTextures[currentIndex].end(),
                          [](MetalTexture *texture) {
                texture->SetInFlight(false);
                texture->readOnlyTexture = nil;
            });
        currentIndex = index;
        EmptyTrash(inFlightTextures);
    }

    bool IsInFlight(MetalTexture *texture, bool previous)
    {
        for (u32 i = 0; i < inFlightTextures.size(); i++)
            if ((!previous || i != currentIndex)
                && inFlightTextures[i].find(texture) != inFlightTextures[i].end())
                return true;
        return false;
    }

    void SetInFlight(MetalTexture *texture)
    {
        texture->SetInFlight(true);
        inFlightTextures[currentIndex].insert(texture);
    }

    void Cleanup();

    void Clear()
    {
        for (auto& set : inFlightTextures)
        {
            for (MetalTexture *tex : set)
                tex->SetInFlight(false);
            set.clear();
        }
        BaseTextureCache::Clear();
    }

private:
    bool clearTexture(MetalTexture *tex)
    {
        for (auto& set : inFlightTextures)
            set.erase(tex);

        return tex->Delete();
    }

    template<typename T>
    void EmptyTrash(T& v)
    {
        if (v.size() < currentIndex + 1)
            v.resize(currentIndex + 1);
        else
            v[currentIndex].clear();
    }

    std::vector<std::unordered_set<MetalTexture *>> inFlightTextures;
    u32 currentIndex = ~0;
};
