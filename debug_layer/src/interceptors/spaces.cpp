// SPDX-License-Identifier: MIT
// interceptors/spaces.cpp — Reference/Action spaces, xrLocateSpace, xrLocateViews.

#include "../dispatch.h"
#include "../instance_data.h"

#include <ctime>
#include <iostream>
#include <string>

namespace debug_layer {

// ── Helper: resolve a path, querying runtime if needed ───────────────────────

static std::string resolve_path_for_space(InstanceData *data, XrPath path)
{
    if (path == XR_NULL_PATH)
        return "";

    {
        std::shared_lock lock(data->state_mutex);
        std::string cached = data->paths.get_string(path);
        if (cached != "<unknown>")
            return cached;
    }

    char buf[256] = {};
    uint32_t count = 0;
    XrResult r = data->next.xrPathToString(data->instance, path, sizeof(buf), &count, buf);
    if (XR_SUCCEEDED(r) && count > 0) {
        std::string s(buf);
        std::unique_lock lock(data->state_mutex);
        data->paths.add(path, s);
        return s;
    }
    return "<unknown>";
}

// ── xrCreateReferenceSpace ───────────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrCreateReferenceSpace(XrSession session,
                                                   const XrReferenceSpaceCreateInfo *createInfo,
                                                   XrSpace *space)
{
    InstanceData *data = GetInstanceDataFromSession(session);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrCreateReferenceSpace(session, createInfo, space);
    if (XR_FAILED(result))
        return result;

    const char *type_str = "UNKNOWN";
    switch (createInfo->referenceSpaceType) {
    case XR_REFERENCE_SPACE_TYPE_VIEW: type_str = "VIEW"; break;
    case XR_REFERENCE_SPACE_TYPE_LOCAL: type_str = "LOCAL"; break;
    case XR_REFERENCE_SPACE_TYPE_STAGE: type_str = "STAGE"; break;
    case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR: type_str = "LOCAL_FLOOR"; break;
    default: break;
    }
    std::cerr << "[XR_APILAYER_DEBUG_gui] xrCreateReferenceSpace: " << type_str << std::endl;

    RegisterSpace(*space, data);

    {
        std::unique_lock lock(data->state_mutex);
        TrackedSpace ts;
        ts.handle = *space;
        ts.session = session;
        ts.kind = SpaceKind::REFERENCE;
        ts.reference_type = createInfo->referenceSpaceType;
        ts.pose_offset = createInfo->poseInReferenceSpace;
        data->spaces[*space] = ts;
    }

    return result;
}

// ── xrCreateActionSpace ──────────────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrCreateActionSpace(XrSession session,
                                               const XrActionSpaceCreateInfo *createInfo,
                                               XrSpace *space)
{
    InstanceData *data = GetInstanceDataFromSession(session);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrCreateActionSpace(session, createInfo, space);
    if (XR_FAILED(result))
        return result;

    // Resolve action name and subaction path
    std::string action_name;
    {
        std::shared_lock lock(data->state_mutex);
        auto ait = data->actions.find(createInfo->action);
        if (ait != data->actions.end())
            action_name = ait->second.name;
    }
    std::string subaction_str = resolve_path_for_space(data, createInfo->subactionPath);

    std::cerr << "[XR_APILAYER_DEBUG_gui] xrCreateActionSpace: " << action_name;
    if (!subaction_str.empty())
        std::cerr << " [" << subaction_str << "]";
    std::cerr << std::endl;

    RegisterSpace(*space, data);

    {
        std::unique_lock lock(data->state_mutex);
        TrackedSpace ts;
        ts.handle = *space;
        ts.session = session;
        ts.kind = SpaceKind::ACTION;
        ts.action = createInfo->action;
        ts.subaction_path = createInfo->subactionPath;
        ts.action_name = action_name;
        ts.subaction_string = subaction_str;
        ts.pose_offset = createInfo->poseInActionSpace;
        data->spaces[*space] = ts;
    }

    return result;
}

// ── xrDestroySpace ───────────────────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrDestroySpace(XrSpace space)
{
    InstanceData *data = GetInstanceDataFromSpace(space);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrDestroySpace(space);

    {
        std::unique_lock lock(data->state_mutex);
        data->spaces.erase(space);
    }
    UnregisterSpace(space);

    return result;
}

// ── xrLocateSpace ────────────────────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time,
                                         XrSpaceLocation *location)
{
    InstanceData *data = GetInstanceDataFromSpace(space);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrLocateSpace(space, baseSpace, time, location);
    if (XR_SUCCEEDED(result)) {
        std::unique_lock lock(data->state_mutex);
        auto it = data->spaces.find(space);
        if (it != data->spaces.end()) {
            it->second.has_location = true;
            it->second.latest_location = *location;
            it->second.located_relative_to = baseSpace;
        }
    }

    return result;
}

// ── xrLocateViews ────────────────────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrLocateViews(XrSession session,
                                         const XrViewLocateInfo *viewLocateInfo,
                                         XrViewState *viewState, uint32_t viewCapacityInput,
                                         uint32_t *viewCountOutput, XrView *views)
{
    InstanceData *data = GetInstanceDataFromSession(session);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    XrResult result =
        data->next.xrLocateViews(session, viewLocateInfo, viewState, viewCapacityInput,
                                 viewCountOutput, views);
    clock_gettime(CLOCK_MONOTONIC, &ts1);

    if (XR_SUCCEEDED(result) && views != nullptr && viewCountOutput != nullptr &&
        viewState != nullptr) {
        bool pos_valid =
            (viewState->viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        bool orient_valid =
            (viewState->viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;

        int64_t t0 = (int64_t)ts0.tv_sec * 1000000000LL + ts0.tv_nsec;
        int64_t t1 = (int64_t)ts1.tv_sec * 1000000000LL + ts1.tv_nsec;

        std::unique_lock lock(data->state_mutex);
        uint32_t count = *viewCountOutput;
        data->view_poses.resize(count);

        for (uint32_t i = 0; i < count; i++) {
            data->view_poses[i].valid = pos_valid && orient_valid;
            data->view_poses[i].pose = views[i].pose;
            data->view_poses[i].fov = views[i].fov;
            data->view_poses[i].base_space = viewLocateInfo->space;
            data->view_poses[i].label = "View " + std::to_string(i);
        }

        if (data->perf.has_current) {
            data->perf.current.locate_views_call_ts = t0;
            data->perf.current.locate_views_return_ts = t1;
        }
    }

    return result;
}

} // namespace debug_layer
