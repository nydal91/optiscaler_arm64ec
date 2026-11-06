// Shared declarations for the HLSL port of Snapdragon Game Super Resolution 2 (2-pass compute variant).
//
// Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//
// Differences from the upstream GLSL:
// - Motion comes from the full per-pixel NGX motion vectors, so the camera reprojection path and its
//   clipToPrevClip matrix are not used.
// - Inverted depth is normalized on read instead of swapping min and max through the depth logic.
// - Y is handled as upstream's REQUEST_NDC_Y_UP, which matches D3D texture coordinates.

// Vulkan builds define SGSR2_VK, the bindings match SGSR2Passes_Vk. Regenerate its headers with build_vk.sh
#ifdef SGSR2_VK
#define VK_BINDING(index) [[vk::binding(index)]]
#define VK_FORMAT(format) [[spv::format_##format]]
#else
#define VK_BINDING(index)
#define VK_FORMAT(format)
#endif

// Must match SGSR2Constants
VK_BINDING(0) cbuffer Params : register(b0)
{
    uint2 renderSize;
    uint2 displaySize;
    float2 renderSizeRcp;
    float2 displaySizeRcp;
    float2 jitterOffset;
    float2 motionUvScale;    // Motion vector texel value to UV offset
    float2 motionTexelScale; // Render pixel to motion vector texel
    float2 jitterCancel;     // UV offset removing jitter from jittered motion vectors
    float2 depthSizeRcp;
    float preExposure;
    float cameraFovAngleHor;
    float minLerpContribution;
    uint sameCamera;
    uint reset;
    uint depthInverted;
};

VK_BINDING(1) SamplerState LinearClamp : register(s0);
VK_BINDING(2) SamplerState PointClamp : register(s1);
