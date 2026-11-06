#pragma once
#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12Utils.h>
#include <shaders/Shader_Dx12.h>

#include "SGSR1Constants.h"

class SGSR1Pass_Dx12 : public Shader_Dx12
{
    // Every frame needs its own heap and constants as the GPU can still be reading the previous ones
    static constexpr uint32_t FramesInFlight = 8;

    FrameDescriptorHeap _heaps[FramesInFlight];
    ID3D12Resource* _frameConstants[FramesInFlight] = {};

  public:
    // The color has to be readable by compute shaders and the output in UNORDERED_ACCESS
    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* color, ID3D12Resource* output,
                  const SGSR1Constants& constants);

    SGSR1Pass_Dx12(std::string InName, ID3D12Device* InDevice);

    ~SGSR1Pass_Dx12();
};
