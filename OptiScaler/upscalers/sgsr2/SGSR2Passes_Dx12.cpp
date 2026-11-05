#include <pch.h>
#include <Config.h>
#include <State.h>
#include "SGSR2Passes_Dx12.h"

#include "generated/sgsr2_convert_cso.h"
#include "generated/sgsr2_upscale_cso.h"

namespace
{
constexpr D3D12_RESOURCE_STATES ReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES WriteState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

D3D12_STATIC_SAMPLER_DESC ClampSampler(UINT shaderRegister, D3D12_FILTER filter)
{
    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = filter;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = shaderRegister;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return sampler;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    return CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
}
} // namespace

SGSR2Passes_Dx12::SGSR2Passes_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    const D3D12_STATIC_SAMPLER_DESC samplers[] = {
        ClampSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR),
        ClampSampler(1, D3D12_FILTER_MIN_MAG_MIP_POINT),
    };

    if (!SetupRootSignature(InDevice, 3, 2, 1, 0, 0, _countof(samplers), samplers))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    constexpr UINT alignTo = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1;
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer((sizeof(SGSR2Constants) + alignTo) & ~alignTo);
    const auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    for (auto& frameConstants : _frameConstants)
    {
        const auto hr = InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          IID_PPV_ARGS(&frameConstants));

        if (FAILED(hr))
        {
            LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, static_cast<unsigned long>(hr));
            return;
        }
    }

    if (!CreateComputePipeline(InDevice, &_pipelineState, sgsr2_convert_cso, sizeof(sgsr2_convert_cso), nullptr) ||
        !CreateComputePipeline(InDevice, &_upscalePipeline, sgsr2_upscale_cso, sizeof(sgsr2_upscale_cso), nullptr))
    {
        LOG_ERROR("[{0}] Failed to create compute pipelines", _name);
        return;
    }

    _init = InitHeaps(InDevice, _heaps, _countof(_heaps));
}

SGSR2Passes_Dx12::~SGSR2Passes_Dx12()
{
    if (State::Instance().isShuttingDown)
        return;

    for (auto& heap : _heaps)
        heap.ReleaseHeaps();

    for (auto& frameConstants : _frameConstants)
        DeferredRelease::Release(frameConstants);

    DeferredRelease::Release(_upscalePipeline);
    DeferredRelease::Release(_motionDepthClip);
    DeferredRelease::Release(_colorLuma);
    DeferredRelease::Release(_history[0]);
    DeferredRelease::Release(_history[1]);
}

bool SGSR2Passes_Dx12::EnsureTextures(ID3D12Resource* InSource, uint32_t renderWidth, uint32_t renderHeight,
                                      uint32_t displayWidth, uint32_t displayHeight, bool& recreated)
{
    recreated = renderWidth != _renderWidth || renderHeight != _renderHeight || displayWidth != _displayWidth ||
                displayHeight != _displayHeight;

    const auto create = [&](ID3D12Resource** texture, uint32_t width, uint32_t height, DXGI_FORMAT format)
    {
        return Shader_Dx12::CreateBufferResource(_device, InSource, WriteState, texture,
                                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, width, height, format);
    };

    if (!create(&_motionDepthClip, renderWidth, renderHeight, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        !create(&_colorLuma, renderWidth, renderHeight, DXGI_FORMAT_R32_UINT) ||
        !create(&_history[0], displayWidth, displayHeight, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        !create(&_history[1], displayWidth, displayHeight, DXGI_FORMAT_R16G16B16A16_FLOAT))
    {
        _renderWidth = 0;
        return false;
    }

    _renderWidth = renderWidth;
    _renderHeight = renderHeight;
    _displayWidth = displayWidth;
    _displayHeight = displayHeight;

    return true;
}

bool SGSR2Passes_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, const Inputs& inputs,
                                const SGSR2Constants& constants)
{
    if (!_init || InCmdList == nullptr || _motionDepthClip == nullptr)
        return false;

    _counter = (_counter + 1) % FramesInFlight;

    FrameDescriptorHeap& convertHeap = _heaps[_counter * HeapsPerFrame];
    FrameDescriptorHeap& upscaleHeap = _heaps[_counter * HeapsPerFrame + 1];
    ID3D12Resource* frameConstants = _frameConstants[_counter];

    ID3D12Resource* prevHistory = _history[_historyIndex];
    ID3D12Resource* nextHistory = _history[_historyIndex ^ 1];

    CreateShaderResourceView(_device, inputs.color, convertHeap.GetSrvCPU(0));
    CreateShaderResourceView(_device, inputs.depth, convertHeap.GetSrvCPU(1));
    CreateShaderResourceView(_device, inputs.velocity, convertHeap.GetSrvCPU(2));
    CreateUnorderedAccessView(_device, _motionDepthClip, convertHeap.GetUavCPU(0), 0);
    CreateUnorderedAccessView(_device, _colorLuma, convertHeap.GetUavCPU(1), 0);

    CreateShaderResourceView(_device, prevHistory, upscaleHeap.GetSrvCPU(0));
    CreateShaderResourceView(_device, _motionDepthClip, upscaleHeap.GetSrvCPU(1));
    CreateShaderResourceView(_device, _colorLuma, upscaleHeap.GetSrvCPU(2));
    CreateUnorderedAccessView(_device, inputs.output, upscaleHeap.GetUavCPU(0), 0);
    CreateUnorderedAccessView(_device, nextHistory, upscaleHeap.GetUavCPU(1), 0);

    if (!CreateConstantsBuffer(_device, frameConstants, constants, convertHeap.GetCbvCPU(0)) ||
        !CreateConstantsBuffer(_device, frameConstants, constants, upscaleHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    {
        ID3D12DescriptorHeap* heaps[] = { convertHeap.GetHeapCSU() };
        InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
        InCmdList->SetComputeRootSignature(_rootSignature);
        InCmdList->SetPipelineState(_pipelineState);
        InCmdList->SetComputeRootDescriptorTable(0, convertHeap.GetTableGPUStart());
        InCmdList->Dispatch((constants.renderSize[0] + SGSR2ThreadGroupSize - 1) / SGSR2ThreadGroupSize,
                            (constants.renderSize[1] + SGSR2ThreadGroupSize - 1) / SGSR2ThreadGroupSize, 1);
    }

    {
        const D3D12_RESOURCE_BARRIER toRead[] = {
            Transition(_motionDepthClip, WriteState, ReadState),
            Transition(_colorLuma, WriteState, ReadState),
            Transition(prevHistory, WriteState, ReadState),
        };
        InCmdList->ResourceBarrier(_countof(toRead), toRead);
    }

    {
        ID3D12DescriptorHeap* heaps[] = { upscaleHeap.GetHeapCSU() };
        InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
        InCmdList->SetComputeRootSignature(_rootSignature);
        InCmdList->SetPipelineState(_upscalePipeline);
        InCmdList->SetComputeRootDescriptorTable(0, upscaleHeap.GetTableGPUStart());
        InCmdList->Dispatch((constants.displaySize[0] + SGSR2ThreadGroupSize - 1) / SGSR2ThreadGroupSize,
                            (constants.displaySize[1] + SGSR2ThreadGroupSize - 1) / SGSR2ThreadGroupSize, 1);
    }

    {
        const D3D12_RESOURCE_BARRIER toWrite[] = {
            Transition(_motionDepthClip, ReadState, WriteState), Transition(_colorLuma, ReadState, WriteState),
            Transition(prevHistory, ReadState, WriteState),      CD3DX12_RESOURCE_BARRIER::UAV(inputs.output),
            CD3DX12_RESOURCE_BARRIER::UAV(nextHistory),
        };
        InCmdList->ResourceBarrier(_countof(toWrite), toWrite);
    }

    _historyIndex ^= 1;

    return true;
}
