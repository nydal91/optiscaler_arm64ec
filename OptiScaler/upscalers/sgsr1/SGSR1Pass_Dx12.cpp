#include <pch.h>
#include <Config.h>
#include <State.h>
#include "SGSR1Pass_Dx12.h"

#include "generated/sgsr1_upscale_cso.h"

SGSR1Pass_Dx12::SGSR1Pass_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    if (!SetupRootSignature(InDevice, 1, 1, 1, 0, 0, 1, &sampler))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    constexpr UINT alignTo = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1;
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer((sizeof(SGSR1Constants) + alignTo) & ~alignTo);
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

    if (!CreateComputePipeline(InDevice, &_pipelineState, sgsr1_upscale_cso, sizeof(sgsr1_upscale_cso), nullptr))
    {
        LOG_ERROR("[{0}] Failed to create the compute pipeline", _name);
        return;
    }

    _init = InitHeaps(InDevice, _heaps, _countof(_heaps));
}

SGSR1Pass_Dx12::~SGSR1Pass_Dx12()
{
    if (State::Instance().isShuttingDown)
        return;

    for (auto& heap : _heaps)
        heap.ReleaseHeaps();

    for (auto& frameConstants : _frameConstants)
        DeferredRelease::Release(frameConstants);
}

bool SGSR1Pass_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* color, ID3D12Resource* output,
                              const SGSR1Constants& constants)
{
    if (!_init || InCmdList == nullptr)
        return false;

    _counter = (_counter + 1) % FramesInFlight;

    FrameDescriptorHeap& heap = _heaps[_counter];
    ID3D12Resource* frameConstants = _frameConstants[_counter];

    CreateShaderResourceView(_device, color, heap.GetSrvCPU(0));
    CreateUnorderedAccessView(_device, output, heap.GetUavCPU(0), 0);

    if (!CreateConstantsBuffer(_device, frameConstants, constants, heap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
    InCmdList->Dispatch((constants.displaySize[0] + SGSR1ThreadGroupSize - 1) / SGSR1ThreadGroupSize,
                        (constants.displaySize[1] + SGSR1ThreadGroupSize - 1) / SGSR1ThreadGroupSize, 1);

    const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(output);
    InCmdList->ResourceBarrier(1, &barrier);

    return true;
}
