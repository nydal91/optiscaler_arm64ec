#include <pch.h>
#include <upscalers/IFeature.h>
#include "SGSR1Constants.h"

SGSR1Constants BuildSGSR1Constants(NVSDK_NGX_Parameter* InParameters, const SGSR1FrameInfo& frame)
{
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitterX);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitterY);

    SGSR1Constants constants {};
    constants.displaySize[0] = frame.displayWidth;
    constants.displaySize[1] = frame.displayHeight;
    constants.displaySizeRcp[0] = 1.0f / static_cast<float>(frame.displayWidth);
    constants.displaySizeRcp[1] = 1.0f / static_cast<float>(frame.displayHeight);
    constants.renderSize[0] = static_cast<float>(frame.renderWidth);
    constants.renderSize[1] = static_cast<float>(frame.renderHeight);
    constants.inputSizeRcp[0] = 1.0f / static_cast<float>(frame.inputWidth);
    constants.inputSizeRcp[1] = 1.0f / static_cast<float>(frame.inputHeight);
    constants.jitterOffset[0] = jitterX;
    constants.jitterOffset[1] = jitterY;
    constants.hdr = frame.hdr ? 1 : 0;

    return constants;
}
