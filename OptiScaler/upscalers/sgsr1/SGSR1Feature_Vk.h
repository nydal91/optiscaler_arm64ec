#pragma once
#include <upscalers/IFeature_Vk.h>

#include "SGSR1Constants.h"
#include "SGSR1Pass_Vk.h"

// Snapdragon Game Super Resolution 1, a spatial upscaler with its own sharpening
class SGSR1FeatureVk : public IFeature_Vk
{
  private:
    std::unique_ptr<SGSR1Pass_Vk> _pass;

  protected:
    bool InitInternal(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters) override;

  public:
    SGSR1FeatureVk(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);

    feature_version Version() override { return { 1, 0, 0 }; }
    Upscaler GetUpscalerType() const final { return Upscaler::SGSR1; }
    bool IsWithDx12() final { return false; }
};
