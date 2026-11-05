#pragma once
#include "SysUtils.h"
#include <d3d12.h>

class D3D12Hooks
{
  private:
    inline static std::mutex hookMutex;
    inline static std::mutex agilityMutex;

  public:
    static void Hook();
    static void HookAgility(HMODULE module);
    static void HookDevice(ID3D12Device* device);
    static void Unhook();

    // Enables/disables command list state recording for the CALLING THREAD.
    // Call with false before Opti records its own work into the game's command list, true afterwards.
    static void SetRootSignatureTracking(bool enable);

    static bool CanRestoreRootSignature(ID3D12GraphicsCommandList* cmdList);
    static void HookToCommandListLate(ID3D12GraphicsCommandList* commandList);
    static void RestoreRoot(ID3D12GraphicsCommandList* cmdList);

    // FSR 2.x picks its FP16 shaders from MinPrecisionSupport alone, which vkd3d-proton then rejects when
    // the device lacks native 16-bit ops. Hides 16-bit precision from CheckFeatureSupport on the CALLING
    // THREAD in that case, so FSR 2.x falls back to its FP32 shaders.
    static void SetHideMinPrecision16(bool enable);
};

class ScopedHideMinPrecision16
{
  public:
    ScopedHideMinPrecision16() { D3D12Hooks::SetHideMinPrecision16(true); }
    ~ScopedHideMinPrecision16() { D3D12Hooks::SetHideMinPrecision16(false); }
};
