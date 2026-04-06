// SPDX-License-Identifier: MIT
// tracked_state.h — Data structures for all OpenXR objects tracked by the layer.
#pragma once

#include <openxr/openxr.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <deque>

namespace debug_layer {

// ── Path cache ───────────────────────────────────────────────────────────────

struct TrackedPath {
    std::unordered_map<XrPath, std::string> to_string;
    std::unordered_map<std::string, XrPath> to_path;

    void add(XrPath p, const std::string &s)
    {
        to_string[p] = s;
        to_path[s] = p;
    }

    std::string get_string(XrPath p) const
    {
        auto it = to_string.find(p);
        return it != to_string.end() ? it->second : "<unknown>";
    }

    XrPath get_path(const std::string &s) const
    {
        auto it = to_path.find(s);
        return it != to_path.end() ? it->second : XR_NULL_PATH;
    }
};

// ── Action sets ──────────────────────────────────────────────────────────────

struct TrackedActionSet {
    XrActionSet handle = XR_NULL_HANDLE;
    std::string name;
    std::string localized_name;
    uint32_t priority = 0;
    bool attached = false;
};

// ── Actions ──────────────────────────────────────────────────────────────────

struct TrackedAction {
    XrAction handle = XR_NULL_HANDLE;
    XrActionSet parent_action_set = XR_NULL_HANDLE;
    std::string name;
    std::string localized_name;
    XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::vector<std::string> subaction_path_strings;
    std::vector<XrPath> subaction_paths;
};

// ── Suggested bindings ───────────────────────────────────────────────────────

struct TrackedBinding {
    XrAction action = XR_NULL_HANDLE;
    std::string binding_path;
};

struct TrackedSuggestedBindings {
    std::string interaction_profile;
    std::vector<TrackedBinding> bindings;
};

// ── Action state (per action + subpath pair) ─────────────────────────────────

struct ActionStateKey {
    XrAction action = XR_NULL_HANDLE;
    XrPath subaction_path = XR_NULL_PATH;

    bool operator==(const ActionStateKey &o) const
    {
        return action == o.action && subaction_path == o.subaction_path;
    }
};

struct ActionStateKeyHash {
    size_t operator()(const ActionStateKey &k) const
    {
        size_t h1 = std::hash<uint64_t>{}(reinterpret_cast<uint64_t>(k.action));
        size_t h2 = std::hash<uint64_t>{}(k.subaction_path);
        return h1 ^ (h2 << 1);
    }
};

struct TrackedActionState {
    XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
    bool is_active = false;
    bool changed_since_last_sync = false;
    XrTime last_change_time = 0;

    // Value storage — use according to `type`
    XrBool32 boolean_value = XR_FALSE;
    float float_value = 0.0f;
    float vector2f_x = 0.0f;
    float vector2f_y = 0.0f;
    XrBool32 pose_is_active = XR_FALSE;
};

// ── Active interaction profiles ──────────────────────────────────────────────

struct TrackedActiveProfile {
    XrPath subaction_path = XR_NULL_PATH;
    XrPath interaction_profile = XR_NULL_PATH;
    std::string profile_string;
    std::string subaction_string;
};

// ── Spaces ───────────────────────────────────────────────────────────────────

enum class SpaceKind { REFERENCE, ACTION };

struct TrackedSpace {
    XrSpace handle = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    SpaceKind kind = SpaceKind::REFERENCE;

    // For REFERENCE spaces
    XrReferenceSpaceType reference_type = XR_REFERENCE_SPACE_TYPE_LOCAL;

    // For ACTION spaces
    XrAction action = XR_NULL_HANDLE;
    XrPath subaction_path = XR_NULL_PATH;
    std::string action_name;
    std::string subaction_string;

    // Pose offset (from create info)
    XrPosef pose_offset = {{0, 0, 0, 1}, {0, 0, 0}};

    // Latest locate result
    bool has_location = false;
    XrSpaceLocation latest_location = {XR_TYPE_SPACE_LOCATION};
    XrSpace located_relative_to = XR_NULL_HANDLE;

    std::string label() const
    {
        if (kind == SpaceKind::REFERENCE) {
            switch (reference_type) {
            case XR_REFERENCE_SPACE_TYPE_VIEW: return "VIEW";
            case XR_REFERENCE_SPACE_TYPE_LOCAL: return "LOCAL";
            case XR_REFERENCE_SPACE_TYPE_STAGE: return "STAGE";
            case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR: return "LOCAL_FLOOR";
            default: return "REF(" + std::to_string(reference_type) + ")";
            }
        } else {
            std::string l = action_name;
            if (!subaction_string.empty())
                l += " [" + subaction_string + "]";
            return l;
        }
    }
};

// ── Session ──────────────────────────────────────────────────────────────────

struct TrackedSession {
    XrSession handle = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    std::vector<XrActionSet> attached_action_sets;
};

// ── Frame timing ─────────────────────────────────────────────────────────────

struct TrackedFrameState {
    XrTime predicted_display_time = 0;
    XrDuration predicted_display_period = 0;
    XrBool32 should_render = XR_FALSE;
    uint64_t frame_count = 0;

    // Ring buffer of consecutive display-time deltas in milliseconds.
    static constexpr size_t kMaxTimingSamples = 2048;
    std::vector<float> timing_deltas_ms = std::vector<float>(kMaxTimingSamples, 0.0f);
    size_t timing_write_idx = 0;
    size_t timing_count = 0;
    XrTime prev_display_time = 0;
};

// ── View poses (from xrLocateViews) ──────────────────────────────────────────

struct TrackedViewPose {
    bool valid = false;
    XrPosef pose = {{0, 0, 0, 1}, {0, 0, 0}};
    XrFovf fov = {0, 0, 0, 0};
    std::string label;
};

// ── Performance timing (per-frame call timings) ──────────────────────────────
// All timestamps are CLOCK_MONOTONIC nanoseconds, consistent with XrTime.

struct SwapchainTiming {
    int64_t acquire_call_ts = 0; // when app called xrAcquireSwapchainImage
    int64_t acquire_done_ts = 0; // when runtime returned from acquire
    int64_t wait_call_ts = 0;    // when app called xrWaitSwapchainImage
    int64_t wait_done_ts = 0;    // when runtime returned from wait
    int64_t release_ts = 0;      // when app called xrReleaseSwapchainImage
    int64_t release_done_ts = 0; // when runtime returned from release
};

struct FramePerfRecord {
    uint64_t frame_number = 0;

    // xrWaitFrame
    int64_t wait_frame_call_ts = 0;    // app calls xrWaitFrame
    int64_t wait_frame_return_ts = 0;  // runtime unblocks (returns)
    XrTime predicted_display_time = 0;
    XrDuration predicted_display_period = 0;

    // xrBeginFrame
    int64_t begin_frame_call_ts = 0;
    int64_t begin_frame_return_ts = 0;

    // Swapchains (one per swapchain used this frame; typically 2 for stereo)
    std::vector<SwapchainTiming> swapchains;

    // xrEndFrame
    int64_t end_frame_call_ts = 0;
    int64_t end_frame_return_ts = 0;

    // xrSyncActions / xrLocateViews (useful to see CPU work between begin/end)
    int64_t sync_actions_call_ts = 0;
    int64_t sync_actions_return_ts = 0;
    int64_t locate_views_call_ts = 0;
    int64_t locate_views_return_ts = 0;

    // Derived durations (computed once when end_frame returns), all in ms.
    float wait_frame_ms = 0.0f;   // how long runtime blocked the app
    float begin_frame_ms = 0.0f;
    float end_frame_ms = 0.0f;
    float app_work_ms = 0.0f;     // begin_frame return → end_frame call (CPU render prep)
    float total_frame_ms = 0.0f;  // wait_frame call → end_frame return (full frame)
    float sync_actions_ms = 0.0f;
    float locate_views_ms = 0.0f;
    float swapchain_acquire_wait_ms = 0.0f; // sum of all acquire+wait across swapchains
};

struct PerfTimeline {
    static constexpr size_t kMaxFrames = 2048;
    std::deque<FramePerfRecord> frames;

    // In-progress frame being built (not yet in `frames`)
    FramePerfRecord current;
    bool has_current = false;

    // Pause state: when paused, new frames still record into current/frames
    // but the GUI reads from a frozen snapshot.
    bool paused = false;
    std::deque<FramePerfRecord> frozen_frames; // snapshot when paused

    void push_completed_frame()
    {
        if (!has_current)
            return;
        frames.push_back(std::move(current));
        if (frames.size() > kMaxFrames)
            frames.pop_front();
        current = {};
        has_current = false;
    }
};

} // namespace debug_layer
