// SPDX-License-Identifier: MIT
// instance_data.h — Per-instance state container and global handle lookup maps.
#pragma once

#include "dispatch.h"
#include "tracked_state.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

namespace debug_layer {

// ── Per-instance data ────────────────────────────────────────────────────────
// One InstanceData exists per XrInstance created through the layer.

struct InstanceData {
    XrInstance instance = XR_NULL_HANDLE;
    NextDispatch next = {};

    // Protects all tracked state below.  GUI thread takes shared_lock (read),
    // interceptors take unique_lock (write).
    mutable std::shared_mutex state_mutex;

    // Tracked state
    TrackedPath paths;
    std::unordered_map<XrActionSet, TrackedActionSet> action_sets;
    std::unordered_map<XrAction, TrackedAction> actions;
    std::vector<TrackedSuggestedBindings> suggested_bindings;
    std::unordered_map<ActionStateKey, TrackedActionState, ActionStateKeyHash> action_states;
    std::vector<TrackedActiveProfile> active_profiles;

    std::unordered_map<XrSession, TrackedSession> sessions;
    std::unordered_map<XrSpace, TrackedSpace> spaces;
    TrackedFrameState frame_state;
    std::vector<TrackedViewPose> view_poses;
    PerfTimeline perf;

    // GUI thread
    std::thread gui_thread;
    std::atomic<bool> gui_running{false};

    // Helpers
    std::string path_to_string(XrPath p) const { return paths.get_string(p); }
};

// ── Global handle maps ───────────────────────────────────────────────────────
// All protected by g_map_mutex.

extern std::mutex g_map_mutex;
extern std::unordered_map<XrInstance, InstanceData *> g_instance_map;
extern std::unordered_map<XrSession, InstanceData *> g_session_map;
extern std::unordered_map<XrActionSet, InstanceData *> g_action_set_map;
extern std::unordered_map<XrAction, InstanceData *> g_action_map;
extern std::unordered_map<XrSpace, InstanceData *> g_space_map;
extern std::unordered_map<XrSwapchain, InstanceData *> g_swapchain_map;

// ── Lookup helpers ───────────────────────────────────────────────────────────

InstanceData *GetInstanceData(XrInstance instance);
InstanceData *GetInstanceDataFromSession(XrSession session);
InstanceData *GetInstanceDataFromActionSet(XrActionSet actionSet);
InstanceData *GetInstanceDataFromAction(XrAction action);
InstanceData *GetInstanceDataFromSpace(XrSpace space);
InstanceData *GetInstanceDataFromSwapchain(XrSwapchain swapchain);

// ── Registration helpers (called from interceptors) ──────────────────────────

void RegisterInstance(XrInstance instance, InstanceData *data);
void UnregisterInstance(XrInstance instance);

void RegisterSession(XrSession session, InstanceData *data);
void UnregisterSession(XrSession session);

void RegisterActionSet(XrActionSet actionSet, InstanceData *data);
void UnregisterActionSet(XrActionSet actionSet);

void RegisterAction(XrAction action, InstanceData *data);
void UnregisterAction(XrAction action);

void RegisterSpace(XrSpace space, InstanceData *data);
void UnregisterSpace(XrSpace space);

void RegisterSwapchain(XrSwapchain swapchain, InstanceData *data);
void UnregisterSwapchain(XrSwapchain swapchain);

} // namespace debug_layer
