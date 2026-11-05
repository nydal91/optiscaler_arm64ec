// HLSL port of sgsr2_convert.comp from Snapdragon Game Super Resolution 2.
//
// Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause

#include "sgsr2_common.hlsli"

Texture2D<float4> InputColor : register(t0);
Texture2D<float> InputDepth : register(t1);
Texture2D<float4> InputVelocity : register(t2);

RWTexture2D<float4> MotionDepthClipAlphaBuffer : register(u0);
RWTexture2D<uint> YCoCgColor : register(u1);

float4 GatherDepth(float2 uv)
{
    const float4 depth = InputDepth.GatherRed(PointClamp, uv);
    return depthInverted != 0 ? 1.0f - depth : depth;
}

float Min4(float4 value)
{
    return min(min(min(value.x, value.y), value.z), value.w);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    const int2 inputPos = int2(dispatchId.xy);

    if (any(dispatchId.xy >= renderSize))
        return;

    // Derived from ffx_fsr2_reconstruct_dilated_velocity_and_previous_depth.h, FindNearestDepth
    const float2 gatherCoord = float2(dispatchId.xy) * depthSizeRcp;
    const float4 topLeft = GatherDepth(gatherCoord);
    const float4 topRight = GatherDepth(gatherCoord + float2(depthSizeRcp.x * 2.0f, 0.0f));
    const float4 bottomLeft = GatherDepth(gatherCoord + float2(0.0f, depthSizeRcp.y * 2.0f));
    const float4 bottomRight = GatherDepth(gatherCoord + depthSizeRcp * 2.0f);

    const float maxC = min(min(min(topLeft.y, topRight.x), bottomLeft.z), bottomRight.w);

    float depthClip = 0.0f;
    if (maxC < 1.0f - 1.0e-05f)
    {
        const float kSep = 1.37e-05f;
        const float depthSep = kSep * cameraFovAngleHor * length(float2(renderSize)) * (1.0f - maxC);
        const float epsilon = 1.19e-07f;

        float wDepth = 0.0f;
        wDepth += saturate(depthSep / (abs(maxC - Min4(topLeft)) + epsilon));
        wDepth += saturate(depthSep / (abs(maxC - Min4(topRight)) + epsilon));
        wDepth += saturate(depthSep / (abs(maxC - Min4(bottomLeft)) + epsilon));
        wDepth += saturate(depthSep / (abs(maxC - Min4(bottomRight)) + epsilon));
        depthClip = saturate(1.0f - wDepth * 0.25f);
    }

    const int2 motionPos = int2((float2(dispatchId.xy) + 0.5f) * motionTexelScale);
    const float2 motionUv = InputVelocity.Load(int3(motionPos, 0)).xy * motionUvScale - jitterCancel;

    // Upstream stores motion in clip space, and its tuning depends on that scale
    const float2 motion = float2(-2.0f * motionUv.x, 2.0f * motionUv.y);

    // Simple tonemap. max() also removes NaNs and negative values from the scene color
    float3 colorRgb = max(InputColor.Load(int3(inputPos, 0)).xyz, 0.0f);
    const float colorMax = max(max(colorRgb.x, colorRgb.y), colorRgb.z) + preExposure;
    colorRgb /= colorMax;

    float3 colorYCoCg;
    colorYCoCg.x = 0.25f * (colorRgb.x + 2.0f * colorRgb.y + colorRgb.z);
    colorYCoCg.y = saturate(0.5f * colorRgb.x + 0.5f - 0.5f * colorRgb.z);
    colorYCoCg.z = saturate(colorYCoCg.x + colorYCoCg.y - colorRgb.x);

    // YCoCg is now in the range of [0, 1]
    const uint x11 = uint(colorYCoCg.x * 2047.5f);
    const uint y11 = uint(colorYCoCg.y * 2047.5f);
    const uint z10 = uint(colorYCoCg.z * 1023.5f);

    YCoCgColor[inputPos] = (x11 << 21u) | (y11 << 10u) | z10;
    MotionDepthClipAlphaBuffer[inputPos] = float4(motion, depthClip, colorMax);
}
