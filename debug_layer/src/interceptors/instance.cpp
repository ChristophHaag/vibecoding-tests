// SPDX-License-Identifier: MIT
// interceptors/instance.cpp — xrDestroyInstance, xrStringToPath, xrPathToString

#include "../dispatch.h"
#include "../gui/gui_main.h"
#include "../instance_data.h"

#include <iostream>

namespace debug_layer {

XrResult XRAPI_CALL Layer_xrDestroyInstance(XrInstance instance)
{
    InstanceData *data = GetInstanceData(instance);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    std::cerr << "[XR_APILAYER_DEBUG_gui] xrDestroyInstance" << std::endl;

    // Stop GUI thread
    gui_stop(data);

    // Forward to next layer
    XrResult result = data->next.xrDestroyInstance(instance);

    // Clean up
    UnregisterInstance(instance);
    delete data;

    return result;
}

XrResult XRAPI_CALL Layer_xrStringToPath(XrInstance instance, const char *pathString, XrPath *path)
{
    InstanceData *data = GetInstanceData(instance);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrStringToPath(instance, pathString, path);

    if (XR_SUCCEEDED(result) && path != nullptr && pathString != nullptr) {
        std::unique_lock lock(data->state_mutex);
        data->paths.add(*path, pathString);
    }

    return result;
}

XrResult XRAPI_CALL Layer_xrPathToString(XrInstance instance, XrPath path,
                                          uint32_t bufferCapacityInput, uint32_t *bufferCountOutput,
                                          char *buffer)
{
    InstanceData *data = GetInstanceData(instance);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result =
        data->next.xrPathToString(instance, path, bufferCapacityInput, bufferCountOutput, buffer);

    if (XR_SUCCEEDED(result) && buffer != nullptr && bufferCapacityInput > 0) {
        std::unique_lock lock(data->state_mutex);
        data->paths.add(path, buffer);
    }

    return result;
}

} // namespace debug_layer
