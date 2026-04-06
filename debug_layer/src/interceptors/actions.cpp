// SPDX-License-Identifier: MIT
// interceptors/actions.cpp — ActionSet/Action/Bindings/Sync/GetState interceptors.

#include "../dispatch.h"
#include "../instance_data.h"

#include <cstring>
#include <iostream>
#include <unordered_set>

namespace debug_layer {

// ── Helper: resolve an XrPath to a string, querying the runtime if needed ────

static std::string resolve_path(InstanceData *data, XrPath path)
{
    if (path == XR_NULL_PATH)
        return "";

    // Check cache first (under shared lock)
    {
        std::shared_lock lock(data->state_mutex);
        std::string cached = data->paths.get_string(path);
        if (cached != "<unknown>")
            return cached;
    }

    // Not cached — query runtime
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

// ── xrCreateActionSet ────────────────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrCreateActionSet(XrInstance instance,
                                             const XrActionSetCreateInfo *createInfo,
                                             XrActionSet *actionSet)
{
    InstanceData *data = GetInstanceData(instance);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrCreateActionSet(instance, createInfo, actionSet);
    if (XR_FAILED(result))
        return result;

    std::cerr << "[XR_APILAYER_DEBUG_gui] xrCreateActionSet: " << createInfo->actionSetName
              << std::endl;

    RegisterActionSet(*actionSet, data);

    {
        std::unique_lock lock(data->state_mutex);
        TrackedActionSet tas;
        tas.handle = *actionSet;
        tas.name = createInfo->actionSetName;
        tas.localized_name = createInfo->localizedActionSetName;
        tas.priority = createInfo->priority;
        data->action_sets[*actionSet] = tas;
    }

    return result;
}

// ── xrDestroyActionSet ───────────────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrDestroyActionSet(XrActionSet actionSet)
{
    InstanceData *data = GetInstanceDataFromActionSet(actionSet);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrDestroyActionSet(actionSet);

    {
        std::unique_lock lock(data->state_mutex);
        data->action_sets.erase(actionSet);
    }
    UnregisterActionSet(actionSet);

    return result;
}

// ── xrCreateAction ───────────────────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrCreateAction(XrActionSet actionSet,
                                          const XrActionCreateInfo *createInfo, XrAction *action)
{
    InstanceData *data = GetInstanceDataFromActionSet(actionSet);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrCreateAction(actionSet, createInfo, action);
    if (XR_FAILED(result))
        return result;

    std::cerr << "[XR_APILAYER_DEBUG_gui] xrCreateAction: " << createInfo->actionName
              << " type=" << createInfo->actionType << std::endl;

    RegisterAction(*action, data);

    {
        std::unique_lock lock(data->state_mutex);
        TrackedAction ta;
        ta.handle = *action;
        ta.parent_action_set = actionSet;
        ta.name = createInfo->actionName;
        ta.localized_name = createInfo->localizedActionName;
        ta.type = createInfo->actionType;

        for (uint32_t i = 0; i < createInfo->countSubactionPaths; i++) {
            ta.subaction_paths.push_back(createInfo->subactionPaths[i]);
        }

        data->actions[*action] = ta;
    }

    // Resolve subaction path strings outside state_mutex to avoid nested locking
    {
        std::vector<std::string> resolved;
        InstanceData *d = data; // avoid capturing structured binding
        auto &ta = data->actions[*action]; // safe: we just inserted it
        for (auto p : ta.subaction_paths) {
            resolved.push_back(resolve_path(d, p));
        }

        std::unique_lock lock(data->state_mutex);
        data->actions[*action].subaction_path_strings = std::move(resolved);
    }

    return result;
}

// ── xrDestroyAction ──────────────────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrDestroyAction(XrAction action)
{
    InstanceData *data = GetInstanceDataFromAction(action);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrDestroyAction(action);

    {
        std::unique_lock lock(data->state_mutex);
        data->actions.erase(action);
    }
    UnregisterAction(action);

    return result;
}

// ── xrSuggestInteractionProfileBindings ──────────────────────────────────────

XrResult XRAPI_CALL Layer_xrSuggestInteractionProfileBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding *suggestedBindings)
{
    InstanceData *data = GetInstanceData(instance);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrSuggestInteractionProfileBindings(instance, suggestedBindings);
    if (XR_FAILED(result))
        return result;

    std::string profile_str = resolve_path(data, suggestedBindings->interactionProfile);
    std::cerr << "[XR_APILAYER_DEBUG_gui] xrSuggestInteractionProfileBindings: " << profile_str
              << " (" << suggestedBindings->countSuggestedBindings << " bindings)" << std::endl;

    TrackedSuggestedBindings tsb;
    tsb.interaction_profile = profile_str;

    for (uint32_t i = 0; i < suggestedBindings->countSuggestedBindings; i++) {
        TrackedBinding tb;
        tb.action = suggestedBindings->suggestedBindings[i].action;
        tb.binding_path = resolve_path(data, suggestedBindings->suggestedBindings[i].binding);
        tsb.bindings.push_back(tb);
    }

    {
        std::unique_lock lock(data->state_mutex);
        // Replace any existing bindings for this profile
        for (auto it = data->suggested_bindings.begin(); it != data->suggested_bindings.end(); ++it) {
            if (it->interaction_profile == profile_str) {
                *it = tsb;
                return result;
            }
        }
        data->suggested_bindings.push_back(tsb);
    }

    return result;
}

// ── xrAttachSessionActionSets ────────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrAttachSessionActionSets(XrSession session,
                                                     const XrSessionActionSetsAttachInfo *attachInfo)
{
    InstanceData *data = GetInstanceDataFromSession(session);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrAttachSessionActionSets(session, attachInfo);
    if (XR_FAILED(result))
        return result;

    std::cerr << "[XR_APILAYER_DEBUG_gui] xrAttachSessionActionSets: "
              << attachInfo->countActionSets << " sets" << std::endl;

    {
        std::unique_lock lock(data->state_mutex);
        auto sit = data->sessions.find(session);
        if (sit != data->sessions.end()) {
            for (uint32_t i = 0; i < attachInfo->countActionSets; i++) {
                sit->second.attached_action_sets.push_back(attachInfo->actionSets[i]);
            }
        }
        for (uint32_t i = 0; i < attachInfo->countActionSets; i++) {
            auto asit = data->action_sets.find(attachInfo->actionSets[i]);
            if (asit != data->action_sets.end())
                asit->second.attached = true;
        }
    }

    return result;
}

// ── refresh_active_profiles ───────────────────────────────────────────────────
// Queries xrGetCurrentInteractionProfile for all top-level user paths observed
// in tracked actions (subaction_paths) and updates data->active_profiles.
// Called from xrSyncActions (every frame) and Layer_xrPollEvent (on event).
// Must NOT be called while holding data->state_mutex.

void refresh_active_profiles(InstanceData *data, XrSession session)
{
    // Collect all distinct subaction paths used by any tracked action.
    std::vector<XrPath> subaction_paths;
    {
        std::shared_lock lock(data->state_mutex);
        std::unordered_set<XrPath> seen;
        for (auto &[h, a] : data->actions) {
            for (XrPath p : a.subaction_paths) {
                if (p != XR_NULL_PATH && seen.insert(p).second)
                    subaction_paths.push_back(p);
            }
        }
    }

    if (subaction_paths.empty())
        return;

    std::vector<TrackedActiveProfile> new_profiles;
    for (XrPath up : subaction_paths) {
        XrInteractionProfileState ps = {XR_TYPE_INTERACTION_PROFILE_STATE};
        if (XR_SUCCEEDED(data->next.xrGetCurrentInteractionProfile(session, up, &ps))) {
            TrackedActiveProfile tap;
            tap.subaction_path = up;
            tap.interaction_profile = ps.interactionProfile;
            tap.subaction_string = resolve_path(data, up);
            tap.profile_string = (ps.interactionProfile != XR_NULL_PATH)
                                     ? resolve_path(data, ps.interactionProfile)
                                     : "<none>";
            new_profiles.push_back(tap);
        }
    }

    std::unique_lock lock(data->state_mutex);
    data->active_profiles = std::move(new_profiles);
}

// ── xrSyncActions ────────────────────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrSyncActions(XrSession session, const XrActionsSyncInfo *syncInfo)
{
    InstanceData *data = GetInstanceDataFromSession(session);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrSyncActions(session, syncInfo);
    if (XR_FAILED(result) && result != XR_SESSION_NOT_FOCUSED)
        return result;

    // After a successful sync, query all tracked action states
    // We need to iterate actions outside the state lock (to call next-layer functions)
    struct ActionQuery {
        XrAction action;
        XrActionType type;
        std::vector<XrPath> subaction_paths;
    };

    std::vector<ActionQuery> queries;
    {
        std::shared_lock lock(data->state_mutex);
        for (auto &[handle, ta] : data->actions) {
            ActionQuery q;
            q.action = handle;
            q.type = ta.type;
            q.subaction_paths = ta.subaction_paths;
            if (q.subaction_paths.empty())
                q.subaction_paths.push_back(XR_NULL_PATH);
            queries.push_back(q);
        }
    }

    // Query each action's state through the next layer
    std::vector<std::pair<ActionStateKey, TrackedActionState>> new_states;

    for (auto &q : queries) {
        for (XrPath subpath : q.subaction_paths) {
            XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
            getInfo.action = q.action;
            getInfo.subactionPath = subpath;

            ActionStateKey key{q.action, subpath};
            TrackedActionState ts;
            ts.type = q.type;

            switch (q.type) {
            case XR_ACTION_TYPE_BOOLEAN_INPUT: {
                XrActionStateBoolean state = {XR_TYPE_ACTION_STATE_BOOLEAN};
                if (XR_SUCCEEDED(data->next.xrGetActionStateBoolean(session, &getInfo, &state))) {
                    ts.is_active = state.isActive == XR_TRUE;
                    ts.boolean_value = state.currentState;
                    ts.changed_since_last_sync = state.changedSinceLastSync == XR_TRUE;
                    ts.last_change_time = state.lastChangeTime;
                }
                break;
            }
            case XR_ACTION_TYPE_FLOAT_INPUT: {
                XrActionStateFloat state = {XR_TYPE_ACTION_STATE_FLOAT};
                if (XR_SUCCEEDED(data->next.xrGetActionStateFloat(session, &getInfo, &state))) {
                    ts.is_active = state.isActive == XR_TRUE;
                    ts.float_value = state.currentState;
                    ts.changed_since_last_sync = state.changedSinceLastSync == XR_TRUE;
                    ts.last_change_time = state.lastChangeTime;
                }
                break;
            }
            case XR_ACTION_TYPE_VECTOR2F_INPUT: {
                XrActionStateVector2f state = {XR_TYPE_ACTION_STATE_VECTOR2F};
                if (XR_SUCCEEDED(
                        data->next.xrGetActionStateVector2f(session, &getInfo, &state))) {
                    ts.is_active = state.isActive == XR_TRUE;
                    ts.vector2f_x = state.currentState.x;
                    ts.vector2f_y = state.currentState.y;
                    ts.changed_since_last_sync = state.changedSinceLastSync == XR_TRUE;
                    ts.last_change_time = state.lastChangeTime;
                }
                break;
            }
            case XR_ACTION_TYPE_POSE_INPUT: {
                XrActionStatePose state = {XR_TYPE_ACTION_STATE_POSE};
                if (XR_SUCCEEDED(data->next.xrGetActionStatePose(session, &getInfo, &state))) {
                    ts.is_active = state.isActive == XR_TRUE;
                    ts.pose_is_active = state.isActive;
                }
                break;
            }
            default: break;
            }

            new_states.push_back({key, ts});
        }
    }

    // Write action states under lock, then refresh interaction profiles.
    {
        std::unique_lock lock(data->state_mutex);
        for (auto &[key, state] : new_states)
            data->action_states[key] = state;
    }

    refresh_active_profiles(data, session);

    return result;
}

// ── xrGetActionState* (pass-through with recording) ──────────────────────────

XrResult XRAPI_CALL Layer_xrGetActionStateBoolean(XrSession session,
                                                    const XrActionStateGetInfo *getInfo,
                                                    XrActionStateBoolean *state)
{
    InstanceData *data = GetInstanceDataFromSession(session);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrGetActionStateBoolean(session, getInfo, state);
    if (XR_SUCCEEDED(result)) {
        ActionStateKey key{getInfo->action, getInfo->subactionPath};
        std::unique_lock lock(data->state_mutex);
        auto &ts = data->action_states[key];
        ts.type = XR_ACTION_TYPE_BOOLEAN_INPUT;
        ts.is_active = state->isActive == XR_TRUE;
        ts.boolean_value = state->currentState;
        ts.changed_since_last_sync = state->changedSinceLastSync == XR_TRUE;
        ts.last_change_time = state->lastChangeTime;
    }
    return result;
}

XrResult XRAPI_CALL Layer_xrGetActionStateFloat(XrSession session,
                                                  const XrActionStateGetInfo *getInfo,
                                                  XrActionStateFloat *state)
{
    InstanceData *data = GetInstanceDataFromSession(session);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrGetActionStateFloat(session, getInfo, state);
    if (XR_SUCCEEDED(result)) {
        ActionStateKey key{getInfo->action, getInfo->subactionPath};
        std::unique_lock lock(data->state_mutex);
        auto &ts = data->action_states[key];
        ts.type = XR_ACTION_TYPE_FLOAT_INPUT;
        ts.is_active = state->isActive == XR_TRUE;
        ts.float_value = state->currentState;
        ts.changed_since_last_sync = state->changedSinceLastSync == XR_TRUE;
        ts.last_change_time = state->lastChangeTime;
    }
    return result;
}

XrResult XRAPI_CALL Layer_xrGetActionStateVector2f(XrSession session,
                                                     const XrActionStateGetInfo *getInfo,
                                                     XrActionStateVector2f *state)
{
    InstanceData *data = GetInstanceDataFromSession(session);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrGetActionStateVector2f(session, getInfo, state);
    if (XR_SUCCEEDED(result)) {
        ActionStateKey key{getInfo->action, getInfo->subactionPath};
        std::unique_lock lock(data->state_mutex);
        auto &ts = data->action_states[key];
        ts.type = XR_ACTION_TYPE_VECTOR2F_INPUT;
        ts.is_active = state->isActive == XR_TRUE;
        ts.vector2f_x = state->currentState.x;
        ts.vector2f_y = state->currentState.y;
        ts.changed_since_last_sync = state->changedSinceLastSync == XR_TRUE;
        ts.last_change_time = state->lastChangeTime;
    }
    return result;
}

XrResult XRAPI_CALL Layer_xrGetActionStatePose(XrSession session,
                                                 const XrActionStateGetInfo *getInfo,
                                                 XrActionStatePose *state)
{
    InstanceData *data = GetInstanceDataFromSession(session);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrGetActionStatePose(session, getInfo, state);
    if (XR_SUCCEEDED(result)) {
        ActionStateKey key{getInfo->action, getInfo->subactionPath};
        std::unique_lock lock(data->state_mutex);
        auto &ts = data->action_states[key];
        ts.type = XR_ACTION_TYPE_POSE_INPUT;
        ts.is_active = state->isActive == XR_TRUE;
        ts.pose_is_active = state->isActive;
    }
    return result;
}

// ── xrGetCurrentInteractionProfile ───────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrGetCurrentInteractionProfile(
    XrSession session, XrPath topLevelUserPath, XrInteractionProfileState *interactionProfile)
{
    InstanceData *data = GetInstanceDataFromSession(session);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    return data->next.xrGetCurrentInteractionProfile(session, topLevelUserPath, interactionProfile);
}

} // namespace debug_layer
