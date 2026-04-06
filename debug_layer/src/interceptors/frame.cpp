// SPDX-License-Identifier: MIT
// interceptors/frame.cpp — xrWaitFrame, xrBeginFrame, xrEndFrame.

#include "../dispatch.h"
#include "../instance_data.h"

namespace debug_layer {

XrResult XRAPI_CALL Layer_xrWaitFrame(XrSession session, const XrFrameWaitInfo *frameWaitInfo,
                                       XrFrameState *frameState)
{
    InstanceData *data = GetInstanceDataFromSession(session);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrWaitFrame(session, frameWaitInfo, frameState);
    if (XR_SUCCEEDED(result) && frameState != nullptr) {
        std::unique_lock lock(data->state_mutex);
        data->frame_state.predicted_display_time = frameState->predictedDisplayTime;
        data->frame_state.predicted_display_period = frameState->predictedDisplayPeriod;
        data->frame_state.should_render = frameState->shouldRender;
        data->frame_state.frame_count++;
    }

    return result;
}

XrResult XRAPI_CALL Layer_xrBeginFrame(XrSession session, const XrFrameBeginInfo *frameBeginInfo)
{
    InstanceData *data = GetInstanceDataFromSession(session);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    return data->next.xrBeginFrame(session, frameBeginInfo);
}

XrResult XRAPI_CALL Layer_xrEndFrame(XrSession session, const XrFrameEndInfo *frameEndInfo)
{
    InstanceData *data = GetInstanceDataFromSession(session);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    // In the future, this is where we'd inspect submitted composition layers
    // and capture swapchain textures for feature 2.

    return data->next.xrEndFrame(session, frameEndInfo);
}

} // namespace debug_layer
