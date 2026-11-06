// HLSL port of sgsr2_upscale.comp from Snapdragon Game Super Resolution 2.
//
// Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause

#include "sgsr2_common.hlsli"

VK_BINDING(3) Texture2D<float4> PrevHistoryOutput : register(t0);
VK_BINDING(4) Texture2D<float4> MotionDepthClipAlphaBuffer : register(t1);
VK_BINDING(5) Texture2D<uint> YCoCgColor : register(t2);

// Its format comes from the game, build_vk.sh turns it into an unknown one as glslang can't declare that
VK_BINDING(6) RWTexture2D<float4> SceneColorOutput : register(u0);
VK_BINDING(7) VK_FORMAT(rgba16f) RWTexture2D<float4> HistoryOutput : register(u1);

struct Accumulator
{
    float4 upsampled;
    float3 boxCenter;
    float3 boxVariance;
    float boxWeight;
    float3 boxMin;
    float3 boxMax;
};

float FastLanczos(float base)
{
    const float y = base - 1.0f;
    const float y2 = y * y;
    const float yTemp = 0.75f * y + y2;
    return yTemp * y2;
}

float3 DecodeColor(uint sample32)
{
    const uint x11 = sample32 >> 21u;
    const uint y11 = sample32 & (2047u << 10u);
    const uint z10 = sample32 & 1023u;

    float3 color;
    color.x = float(x11) * (1.0f / 2047.5f);
    color.y = float(y11) * 4.76953602e-7f - 0.5f;
    color.z = float(z10) * (1.0f / 1023.5f) - 0.5f;
    return color;
}

void AddSample(inout Accumulator acc, uint encoded, float2 offset, float kernelBias2, float curveBias)
{
    const float3 color = DecodeColor(encoded);
    const float offsetDot = dot(offset, offset);
    const float weight = FastLanczos(saturate(offsetDot * kernelBias2));
    const float boxWeight = exp(offsetDot * curveBias);
    const float3 weighted = color * boxWeight;

    acc.upsampled += float4(color * weight, weight);
    acc.boxMin = min(acc.boxMin, color);
    acc.boxMax = max(acc.boxMax, color);
    acc.boxCenter += weighted;
    acc.boxVariance += color * weighted;
    acc.boxWeight += boxWeight;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    if (any(dispatchId.xy >= displaySize))
        return;

    const float2 displayScale = float2(displaySize) / float2(renderSize);
    const float biasMaxViewportXScale = min(displayScale.x, 1.99f);
    const float scaleFactor = min(20.0f, pow(displayScale.x * displayScale.y, 3.0f));

    const float2 historySize = float2(displaySize);
    const float2 inputSize = float2(renderSize);
    const float2 hrUv = (float2(dispatchId.xy) + 0.5f) * displaySizeRcp;
    const float2 jitterUv = saturate(hrUv + jitterOffset * renderSizeRcp);

    const int2 inputPos = int2(jitterUv * inputSize);
    const float4 mda = MotionDepthClipAlphaBuffer.SampleLevel(LinearClamp, jitterUv, 0.0f);
    const float2 motion = mda.xy;

    const float2 prevUv = saturate(float2(-0.5f * motion.x, 0.5f * motion.y) + hrUv);

    const float depthFactor = mda.z;
    const float colorMax = mda.w;

    const float4 history = PrevHistoryOutput.SampleLevel(LinearClamp, prevUv, 0.0f);
    float3 historyColor = history.xyz;
    const float wFactor = saturate(abs(history.w));

    const float kernelFactor = saturate(wFactor + float(reset));
    const float biasMax = biasMaxViewportXScale - biasMaxViewportXScale * kernelFactor;
    const float biasMin = max(1.0f, 0.3f + 0.3f * biasMax);
    const float biasFactor = max(0.25f * depthFactor, kernelFactor);
    const float motionViewportLen = length(motion * historySize);
    const float curveBias = lerp(-2.0f, -3.0f, saturate(motionViewportLen * 0.02f));

    const float2 srcPos = float2(inputPos) + 0.5f - jitterOffset;
    const float2 srcOutputPos = hrUv * inputSize;
    const float2 offset = srcPos - srcOutputPos;

    const float kernelBias = lerp(biasMax, biasMin, biasFactor) * 0.5f;
    const float kernelBias2 = kernelBias * kernelBias;

    const float2 gatherCoord = float2(inputPos) * renderSizeRcp;
    const uint4 topLeft = YCoCgColor.GatherRed(PointClamp, gatherCoord);
    const int2 lastPixel = int2(renderSize) - 1;
    const uint bottomRightSample = YCoCgColor.Load(int3(min(inputPos + 1, lastPixel), 0));

    uint2 topRight;
    uint2 bottomLeft;

    if (sameCamera != 0u)
    {
        topRight = YCoCgColor.GatherRed(PointClamp, gatherCoord + float2(renderSizeRcp.x, 0.0f)).yz;
        bottomLeft = YCoCgColor.GatherRed(PointClamp, gatherCoord + float2(0.0f, renderSizeRcp.y)).xy;
    }
    else
    {
        const uint2 bottomRightGather = YCoCgColor.GatherRed(PointClamp, gatherCoord + renderSizeRcp).xz;
        bottomLeft = uint2(0u, bottomRightGather.x);
        topRight = uint2(bottomRightGather.y, 0u);
    }

    Accumulator acc;
    acc.upsampled = 0.0f;
    acc.boxCenter = 0.0f;
    acc.boxVariance = 0.0f;
    acc.boxWeight = 0.0f;
    acc.boxMin = 1.0e30f;
    acc.boxMax = -1.0e30f;

    AddSample(acc, bottomLeft.y, offset + float2(0.0f, 1.0f), kernelBias2, curveBias);
    AddSample(acc, topRight.x, offset + float2(1.0f, 0.0f), kernelBias2, curveBias);
    AddSample(acc, topLeft.x, offset + float2(-1.0f, 0.0f), kernelBias2, curveBias);
    AddSample(acc, topLeft.y, offset, kernelBias2, curveBias);
    AddSample(acc, topLeft.z, offset + float2(0.0f, -1.0f), kernelBias2, curveBias);

    if (sameCamera != 0u)
    {
        AddSample(acc, bottomRightSample, offset + float2(1.0f, 1.0f), kernelBias2, curveBias);
        AddSample(acc, bottomLeft.x, offset + float2(-1.0f, 1.0f), kernelBias2, curveBias);
        AddSample(acc, topRight.y, offset + float2(1.0f, -1.0f), kernelBias2, curveBias);
        AddSample(acc, topLeft.w, offset + float2(-1.0f, -1.0f), kernelBias2, curveBias);
    }

    const float boxWeightRcp = 1.0f / acc.boxWeight;
    const float3 boxCenter = acc.boxCenter * boxWeightRcp;
    const float3 boxVariance = sqrt(abs(acc.boxVariance * boxWeightRcp - boxCenter * boxCenter));

    float4 upsampled = acc.upsampled;
    upsampled.xyz = clamp(upsampled.xyz / upsampled.w, acc.boxMin - 0.05f, acc.boxMax + 0.05f);
    upsampled.w *= 1.0f / 3.0f;

    const float oneMinusWFactor = 1.0f - wFactor;
    float baseUpdate = oneMinusWFactor - oneMinusWFactor * depthFactor;
    baseUpdate = min(baseUpdate, lerp(baseUpdate, upsampled.w * 10.0f, saturate(10.0f * motionViewportLen)));
    baseUpdate = min(baseUpdate, lerp(baseUpdate, upsampled.w, saturate(motionViewportLen * 0.05f)));
    float baseAlpha = baseUpdate;

    const float epsilon = 1.192e-07f;
    const float boxScale = max(depthFactor, saturate(motionViewportLen * 0.05f));
    const float boxSize = lerp(scaleFactor, 1.0f, boxScale);
    const float3 scaledVariance = boxVariance * boxSize;
    const float3 rectBoxMin = max(acc.boxMin, boxCenter - scaledVariance);
    const float3 rectBoxMax = min(acc.boxMax, boxCenter + scaledVariance);

    const float3 clampedColor = clamp(historyColor, rectBoxMin, rectBoxMax);
    float startLerpValue = minLerpContribution;
    if ((abs(mda.x) + abs(mda.y)) > 0.000001f)
        startLerpValue = 0.0f;

    const bool outsideBox = any(rectBoxMin > historyColor) || any(historyColor > rectBoxMax);
    const float lerpContribution = saturate(outsideBox ? startLerpValue : 1.0f);

    historyColor = lerp(clampedColor, historyColor, lerpContribution);
    baseAlpha = lerp(min(baseAlpha, 0.1f), baseAlpha, lerpContribution);

    const float alphaSum = max(epsilon, baseAlpha + upsampled.w);
    const float alpha = saturate(upsampled.w / alphaSum + float(reset));
    upsampled.xyz = lerp(historyColor, upsampled.xyz, alpha);

    HistoryOutput[dispatchId.xy] = float4(upsampled.xyz, wFactor);

    // YCoCg to RGB
    const float xz = upsampled.x - upsampled.z;
    const float3 rgb = saturate(float3(xz + upsampled.y, upsampled.x + upsampled.z, xz - upsampled.y));

    const float compMax = saturate(max(max(rgb.x, rgb.y), rgb.z));
    float scale = preExposure / ((1.0f + 600.0f / 65504.0f) - compMax);
    if (colorMax > 4000.0f)
        scale = colorMax;

    SceneColorOutput[dispatchId.xy] = float4(rgb * scale, 1.0f);
}
