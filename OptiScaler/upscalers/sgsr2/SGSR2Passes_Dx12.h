#pragma once
#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12Utils.h>
#include <shaders/Shader_Dx12.h>

#include "SGSR2Constants.h"

// The Convert and Upscale passes, which share a root signature and swap two history textures
class SGSR2Passes_Dx12 : public Shader_Dx12
{
  public:
    struct Inputs
    {
        ID3D12Resource* color = nullptr;
        ID3D12Resource* depth = nullptr;
        ID3D12Resource* velocity = nullptr;
        ID3D12Resource* output = nullptr;
    };

  private:
    // Every frame needs its own heaps and constants as the GPU can still be reading the previous ones
    static constexpr uint32_t FramesInFlight = 8;
    static constexpr uint32_t HeapsPerFrame = 2;

    FrameDescriptorHeap _heaps[FramesInFlight * HeapsPerFrame];
    ID3D12Resource* _frameConstants[FramesInFlight] = {};

    ID3D12PipelineState* _upscalePipeline = nullptr;

    // All of them stay in UNORDERED_ACCESS between frames
    ID3D12Resource* _motionDepthClip = nullptr;
    ID3D12Resource* _colorLuma = nullptr;
    ID3D12Resource* _history[2] = {};

    uint32_t _renderWidth = 0;
    uint32_t _renderHeight = 0;
    uint32_t _displayWidth = 0;
    uint32_t _displayHeight = 0;
    uint32_t _historyIndex = 0;

  public:
    // Render and display sizes come from constants, the source only provides heap properties
    bool EnsureTextures(ID3D12Resource* InSource, uint32_t renderWidth, uint32_t renderHeight, uint32_t displayWidth,
                        uint32_t displayHeight, bool& recreated);

    // Inputs have to be readable by compute shaders and the output in UNORDERED_ACCESS
    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, const Inputs& inputs, const SGSR2Constants& constants);

    SGSR2Passes_Dx12(std::string InName, ID3D12Device* InDevice);

    ~SGSR2Passes_Dx12();
};
