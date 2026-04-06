// SPDX-License-Identifier: MIT
// layer_main.cpp — Layer entry point: negotiate, create instance, GetInstanceProcAddr.

#include "dispatch.h"
#include "gui/gui_main.h"
#include "instance_data.h"

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

// ── Visibility ───────────────────────────────────────────────────────────────

#if defined(__GNUC__) || defined(__SUNPRO_C)
#define LAYER_EXPORT __attribute__((visibility("default")))
#elif defined(_WIN32)
#define LAYER_EXPORT __declspec(dllexport)
#else
#define LAYER_EXPORT
#endif

static const char *LAYER_NAME = "XR_APILAYER_DEBUG_gui";

namespace debug_layer {

// ── Layer_xrGetInstanceProcAddr ──────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrGetInstanceProcAddr(XrInstance instance, const char *name,
                                                  PFN_xrVoidFunction *function)
{
    // Check if we intercept this function
    PFN_xrVoidFunction interceptor = GetInterceptor(name);
    if (interceptor != nullptr) {
        *function = interceptor;
        return XR_SUCCESS;
    }

    // Forward to the next layer/runtime
    InstanceData *data = GetInstanceData(instance);
    if (data == nullptr || data->next.GetInstanceProcAddr == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    return data->next.GetInstanceProcAddr(instance, name, function);
}

// ── xrCreateApiLayerInstance ─────────────────────────────────────────────────

static XrResult XRAPI_CALL Layer_xrCreateApiLayerInstance(const XrInstanceCreateInfo *info,
                                                           const XrApiLayerCreateInfo *apiLayerInfo,
                                                           XrInstance *instance)
{
    // ── Validate the layer create info chain ─────────────────────────────
    if (apiLayerInfo == nullptr ||
        apiLayerInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        apiLayerInfo->structVersion < XR_API_LAYER_CREATE_INFO_STRUCT_VERSION ||
        apiLayerInfo->structSize < sizeof(XrApiLayerCreateInfo) ||
        apiLayerInfo->nextInfo == nullptr ||
        apiLayerInfo->nextInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
        apiLayerInfo->nextInfo->structVersion < XR_API_LAYER_NEXT_INFO_STRUCT_VERSION ||
        apiLayerInfo->nextInfo->structSize < sizeof(XrApiLayerNextInfo) ||
        std::strcmp(LAYER_NAME, apiLayerInfo->nextInfo->layerName) != 0 ||
        apiLayerInfo->nextInfo->nextGetInstanceProcAddr == nullptr ||
        apiLayerInfo->nextInfo->nextCreateApiLayerInstance == nullptr) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    // ── Save next-layer pointers, advance the chain ──────────────────────
    PFN_xrGetInstanceProcAddr next_get_proc = apiLayerInfo->nextInfo->nextGetInstanceProcAddr;
    PFN_xrCreateApiLayerInstance next_create = apiLayerInfo->nextInfo->nextCreateApiLayerInstance;

    XrApiLayerCreateInfo next_layer_info = *apiLayerInfo;
    next_layer_info.nextInfo = apiLayerInfo->nextInfo->next;

    // ── Create the instance through the next layer/runtime ───────────────
    XrResult result = next_create(info, &next_layer_info, instance);
    if (XR_FAILED(result))
        return result;

    // ── Build our per-instance data ──────────────────────────────────────
    auto *data = new InstanceData();
    data->instance = *instance;
    PopulateNextDispatch(data->next, *instance, next_get_proc);

    RegisterInstance(*instance, data);

    std::cerr << "[XR_APILAYER_DEBUG_gui] Instance created, layer active." << std::endl;

    // GUI thread is started lazily from xrCreateSession, not here,
    // to avoid interfering with the app's own windowing/GL init.

    return XR_SUCCESS;
}

} // namespace debug_layer

// ── Exported negotiation function ────────────────────────────────────────────

extern "C" LAYER_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo *loaderInfo, const char * /*layerName*/,
    XrNegotiateApiLayerRequest *apiLayerRequest)
{
    debug_layer::RegisterAllInterceptors();

    // Validate loader info
    if (loaderInfo == nullptr ||
        loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo)) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (loaderInfo->minApiVersion > XR_CURRENT_API_VERSION ||
        loaderInfo->maxApiVersion < XR_CURRENT_API_VERSION) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    // Validate request struct
    if (apiLayerRequest == nullptr ||
        apiLayerRequest->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        apiLayerRequest->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        apiLayerRequest->structSize != sizeof(XrNegotiateApiLayerRequest)) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    // Fill in our response
    apiLayerRequest->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    apiLayerRequest->layerApiVersion = XR_CURRENT_API_VERSION;
    apiLayerRequest->getInstanceProcAddr =
        reinterpret_cast<PFN_xrGetInstanceProcAddr>(debug_layer::Layer_xrGetInstanceProcAddr);
    apiLayerRequest->createApiLayerInstance =
        reinterpret_cast<PFN_xrCreateApiLayerInstance>(debug_layer::Layer_xrCreateApiLayerInstance);

    std::cerr << "[XR_APILAYER_DEBUG_gui] Layer negotiated successfully." << std::endl;

    return XR_SUCCESS;
}
