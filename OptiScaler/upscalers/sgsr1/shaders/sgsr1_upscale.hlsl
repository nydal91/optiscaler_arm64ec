// Compute port of sgsr1_mobile.h and sgsr1_shader_mobile.hlsl from Snapdragon Game Super Resolution 1, RGBA mode.
//
// Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//
// Differences from upstream:
// - Upstream expects a display referred input. HDR scene color goes through a reversible tonemap around the filter.
// - NGX hands over jittered frames, the sample position follows the jitter so the picture doesn't shake.
// - The input texture can be larger than the rendered area.

// Vulkan builds define SGSR1_VK, the bindings match SGSR1Pass_Vk. Regenerate its header with build_vk.sh
#ifdef SGSR1_VK
#define VK_BINDING(index) [[vk::binding(index)]]
#else
#define VK_BINDING(index)
#endif

// Must match SGSR1Constants
VK_BINDING(0) cbuffer Params : register(b0)
{
    uint2 displaySize;
    float2 displaySizeRcp;
    float2 renderSize;
    float2 inputSizeRcp;
    float2 jitterOffset;
    uint hdr;
    uint padding;
};

VK_BINDING(1) SamplerState LinearClamp : register(s0);
VK_BINDING(2) Texture2D<float4> InputColor : register(t0);

// Its format comes from the game, build_vk.sh turns it into an unknown one as glslang can't declare that
VK_BINDING(3) RWTexture2D<float4> SceneColorOutput : register(u0);

static const float EdgeThreshold = 8.0f / 255.0f;
static const float EdgeSharpness = 2.0f;

float3 ToFilterRange(float3 color)
{
    return hdr != 0 ? color / (1.0f + color) : saturate(color);
}

float3 FromFilterRange(float3 color)
{
    return hdr != 0 ? color / max(1.0f - color, 0.001f) : color;
}

// The filter only looks at green, as upstream's RGBA mode
float4 GatherLuma(float2 uv)
{
    const float4 green = InputColor.GatherGreen(LinearClamp, uv);
    return hdr != 0 ? max(green, 0.0f) / (1.0f + max(green, 0.0f)) : saturate(green);
}

float FastLanczos2(float x)
{
    float wA = x - 4.0f;
    const float wB = x * wA - wA;
    wA *= wA;
    return wB * wA;
}

float2 WeightY(float dx, float dy, float c, float std)
{
    const float x = (dx * dx + dy * dy) * 0.5f + saturate(abs(c) * std);
    const float w = FastLanczos2(x);
    return float2(w, w * c);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    if (any(dispatchId.xy >= displaySize))
        return;

    const float2 displayUv = (float2(dispatchId.xy) + 0.5f) * displaySizeRcp;
    const float2 samplePos = clamp(displayUv * renderSize + jitterOffset, 0.5f, renderSize - 0.5f);

    float3 color = max(InputColor.SampleLevel(LinearClamp, samplePos * inputSizeRcp, 0.0f).xyz, 0.0f);
    const float3 filterColor = ToFilterRange(color);
    const float center = filterColor.y;

    const float2 imgCoord = samplePos + float2(-0.5f, 0.5f);
    const float2 imgCoordPixel = floor(imgCoord);
    float2 coord = imgCoordPixel * inputSizeRcp;
    const float2 pl = imgCoord - imgCoordPixel;

    float4 left = GatherLuma(coord);

    const float edgeVote = abs(left.z - left.y) + abs(center - left.y) + abs(center - left.z);
    if (edgeVote > EdgeThreshold)
    {
        coord.x += inputSizeRcp.x;

        float4 right = GatherLuma(coord + float2(inputSizeRcp.x, 0.0f));
        float4 upDown;
        upDown.xy = GatherLuma(coord + float2(0.0f, -inputSizeRcp.y)).wz;
        upDown.zw = GatherLuma(coord + float2(0.0f, inputSizeRcp.y)).yx;

        const float mean = (left.y + left.z + right.x + right.w) * 0.25f;
        left -= mean;
        right -= mean;
        upDown -= mean;
        const float centerOffset = center - mean;

        const float sum = abs(left.x) + abs(left.y) + abs(left.z) + abs(left.w) + abs(right.x) + abs(right.y) +
                          abs(right.z) + abs(right.w) + abs(upDown.x) + abs(upDown.y) + abs(upDown.z) + abs(upDown.w);
        const float sumMean = 10.14185f / max(sum, 0.0001f);
        const float std = sumMean * sumMean;

        float2 aWY = WeightY(pl.x, pl.y + 1.0f, upDown.x, std);
        aWY += WeightY(pl.x - 1.0f, pl.y + 1.0f, upDown.y, std);
        aWY += WeightY(pl.x - 1.0f, pl.y - 2.0f, upDown.z, std);
        aWY += WeightY(pl.x, pl.y - 2.0f, upDown.w, std);
        aWY += WeightY(pl.x + 1.0f, pl.y - 1.0f, left.x, std);
        aWY += WeightY(pl.x, pl.y - 1.0f, left.y, std);
        aWY += WeightY(pl.x, pl.y, left.z, std);
        aWY += WeightY(pl.x + 1.0f, pl.y, left.w, std);
        aWY += WeightY(pl.x - 1.0f, pl.y - 1.0f, right.x, std);
        aWY += WeightY(pl.x - 2.0f, pl.y - 1.0f, right.y, std);
        aWY += WeightY(pl.x - 2.0f, pl.y, right.z, std);
        aWY += WeightY(pl.x - 1.0f, pl.y, right.w, std);

        const float maxOf4 = max(max(left.y, left.z), max(right.x, right.w));
        const float minOf4 = min(min(left.y, left.z), min(right.x, right.w));
        const float finalY = clamp(EdgeSharpness * aWY.y / aWY.x, minOf4, maxOf4);

        color = FromFilterRange(saturate(filterColor + (finalY - centerOffset)));
    }

    SceneColorOutput[dispatchId.xy] = float4(color, 1.0f);
}
