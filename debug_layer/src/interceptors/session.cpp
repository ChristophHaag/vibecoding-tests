// SPDX-License-Identifier: MIT
// interceptors/session.cpp — xrCreate/Destroy/Begin/EndSession, xrPollEvent

#include "../dispatch.h"
#include "../gui/gui_main.h"
#include "../instance_data.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace debug_layer {

// Forward declaration of helper defined in actions.cpp.
void refresh_active_profiles(InstanceData *data, XrSession session);

XrResult XRAPI_CALL Layer_xrCreateSession(XrInstance instance,
                                           const XrSessionCreateInfo *createInfo,
                                           XrSession *session)
{
    InstanceData *data = GetInstanceData(instance);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrCreateSession(instance, createInfo, session);
    if (XR_FAILED(result))
        return result;

    std::cerr << "[XR_APILAYER_DEBUG_gui] xrCreateSession" << std::endl;

    // Register the session→instance mapping
    RegisterSession(*session, data);

    {
        std::unique_lock lock(data->state_mutex);
        TrackedSession ts;
        ts.handle = *session;
        ts.state = XR_SESSION_STATE_UNKNOWN;
        data->sessions[*session] = ts;
    }

    // Launch GUI thread (deferred to here to avoid interfering with app's windowing init)
    const char *disable_env = std::getenv("XR_DEBUG_GUI_DISABLE");
    if (disable_env == nullptr || std::strcmp(disable_env, "1") != 0) {
        gui_start(data);
    }

    return result;
}

XrResult XRAPI_CALL Layer_xrDestroySession(XrSession session)
{
    InstanceData *data = GetInstanceDataFromSession(session);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    std::cerr << "[XR_APILAYER_DEBUG_gui] xrDestroySession" << std::endl;

    XrResult result = data->next.xrDestroySession(session);

    {
        std::unique_lock lock(data->state_mutex);

        // Remove all spaces belonging to this session
        for (auto it = data->spaces.begin(); it != data->spaces.end();) {
            if (it->second.session == session) {
                UnregisterSpace(it->first);
                it = data->spaces.erase(it);
            } else {
                ++it;
            }
        }

        data->sessions.erase(session);
    }

    UnregisterSession(session);

    return result;
}

XrResult XRAPI_CALL Layer_xrBeginSession(XrSession session, const XrSessionBeginInfo *beginInfo)
{
    InstanceData *data = GetInstanceDataFromSession(session);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    return data->next.xrBeginSession(session, beginInfo);
}

XrResult XRAPI_CALL Layer_xrEndSession(XrSession session)
{
    InstanceData *data = GetInstanceDataFromSession(session);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    return data->next.xrEndSession(session);
}

// ── xrPollEvent ──────────────────────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrPollEvent(XrInstance instance, XrEventDataBuffer *eventData)
{
    InstanceData *data = GetInstanceData(instance);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrPollEvent(instance, eventData);
    if (result != XR_SUCCESS)
        return result;

    switch (eventData->type) {
    case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
        // Track session state properly via events rather than guessing from API calls.
        auto *ev = reinterpret_cast<const XrEventDataSessionStateChanged *>(eventData);
        std::unique_lock lock(data->state_mutex);
        auto it = data->sessions.find(ev->session);
        if (it != data->sessions.end())
            it->second.state = ev->state;
        break;
    }
    case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
        // Refresh active profiles immediately when the runtime notifies us.
        auto *ev = reinterpret_cast<const XrEventDataInteractionProfileChanged *>(eventData);
        refresh_active_profiles(data, ev->session);
        break;
    }
    default: break;
    }

    return result;
}

} // namespace debug_layer
