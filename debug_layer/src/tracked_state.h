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

} // namespace debug_layer
