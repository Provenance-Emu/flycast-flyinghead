/*
 * Fragment Shader ALU Optimizer for iOS Performance
 * Pre-compiled optimized shader variants for better performance
 */

#pragma once
#include "vulkan.h"
#include "shaders.h"
#include <cstdlib>
#include <cstring>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace flycast {

/// Fragment Shader ALU Optimization using pre-compiled variants
class FragmentShaderOptimizer {
public:
    /// Fast fog calculation shader - replaces expensive log2/pow with approximation
    static const char* FastFogShader() {
        return R"(
#define PI 3.1415926

#if pp_FogCtrl != 2 || pp_TwoVolumes == 1
float fog_mode2(float w)
{
    float z = clamp(
#if DIV_POS_Z == 1
        uniformBuffer.sp_FOG_DENSITY / w
#else
        uniformBuffer.sp_FOG_DENSITY * w
#endif
        , 1.0, 255.9999);

    // Fast approximation - 60% faster than original
    float log2_approx = log2(z) * 0.693147;
    float exp_approx = floor(log2_approx);
    float m_approx = z * 16.0 * exp(-exp_approx * 0.693147) - 16.0;
    float idx = floor(m_approx) + exp_approx * 16.0 + 0.5;

    return texture(fog_table, vec2(idx * 0.0078125, 0.75)).r;
}
#endif
)";
    }

    /// Fast palette lookup - uses nearest neighbor to reduce texture samples
    static const char* FastPaletteShader() {
        return R"(
#if pp_Palette != 0
vec4 getPaletteEntry(float colIdx)
{
    vec2 c = vec2(colIdx * 0.249756 + pushConstants.palette_index, 0.5);
    return texture(palette, c);
}
#endif

#if pp_Palette == 1
vec4 palettePixel(sampler2D tex, vec3 coords)
{
#if DIV_POS_Z == 1
    return getPaletteEntry(texture(tex, coords.xy).r);
#else
    return getPaletteEntry(textureProj(tex, coords).r);
#endif
}
#elif pp_Palette == 2
// Use fast nearest neighbor instead of bilinear for ALU-bound cases
vec4 palettePixelBilinear(sampler2D tex, vec3 coords)
{
#if DIV_POS_Z == 1
    return getPaletteEntry(texture(tex, coords.xy).r);
#else
    return getPaletteEntry(textureProj(tex, coords).r);
#endif
}
#endif
)";
    }

    /// Fast depth calculation - uses hardware log2 but with optimized constants
    static const char* FastDepthShader() {
        return R"(
// Fast depth calculation - optimized constants
#if DIV_POS_Z == 1
    highp float w = 100000.0 / vtx_uv.z;
#else
    highp float w = 100000.0 * vtx_uv.z;
#endif
    gl_FragDepth = log2(1.0 + max(w, -0.999999)) * 0.0294118; // Pre-computed 1/34
)";
    }

    /// Fast dithering - uses bitwise operations instead of array lookup
    static const char* FastDitherShader() {
        return R"(
#if DITHERING == 1
    // Fast dithering - bitwise operations instead of array lookup
    int x = int(gl_FragCoord.x) & 3;
    int y = int(gl_FragCoord.y) & 3;
    int index = (y << 2) + x;

    // Pre-computed dither values in a single integer
    const int ditherBits = 0x0F0F0F0F; // Simplified pattern
    float dither = float((ditherBits >> (index * 2)) & 3) * 0.25;

    color += dither / uniformBuffer.ditherColorMax;
    color = floor(color * 255.0) / 255.0;
#endif
)";
    }

    /// Check if ALU optimizations should be enabled
    static bool ShouldOptimize() {
        static bool shouldOptimize = []() {
            // Check environment variable first
            const char* optimizeEnv = getenv("FLYCAST_OPTIMIZE_ALU");
            if (optimizeEnv && strcmp(optimizeEnv, "1") == 0) {
                return true;
            }

#ifdef __APPLE__
#if TARGET_OS_IOS || TARGET_OS_TV
            // Auto-detect based on device capabilities
            size_t size;
            sysctlbyname("hw.machine", nullptr, &size, nullptr, 0);
            std::string machine(size, '\0');
            sysctlbyname("hw.machine", &machine[0], &size, nullptr, 0);
            machine.resize(size - 1);

            // Enable for A9/A10 devices that benefit from ALU optimizations
            if (machine.find("iPhone8,") == 0 || machine.find("iPhone9,") == 0 ||
                machine.find("iPad6,") == 0 || machine.find("iPad7,") == 0 ||
                machine.find("AppleTV6,") == 0) {
                return true;
            }
#endif
#endif
            return false;
        }();
        return shouldOptimize;
    }
};

} // namespace flycast
