// SPDX-License-Identifier: MIT
// tracked_state.h — Data structures for all OpenXR objects tracked by the
// layer.
#pragma once

#include <openxr/openxr.h>

#include <cstdint>
#include <deque>
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

  void add(XrPath p, const std::string &s) {
    to_string[p] = s;
    to_path[s] = p;
  }

  std::string get_string(XrPath p) const {
    auto it = to_string.find(p);
    return it != to_string.end() ? it->second : "<unknown>";
  }

  XrPath get_path(const std::string &s) const {
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

  bool operator==(const ActionStateKey &o) const {
    return action == o.action && subaction_path == o.subaction_path;
  }
};

struct ActionStateKeyHash {
  size_t operator()(const ActionStateKey &k) const {
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

  std::string label() const {
    if (kind == SpaceKind::REFERENCE) {
      switch (reference_type) {
      case XR_REFERENCE_SPACE_TYPE_VIEW:
        return "VIEW";
      case XR_REFERENCE_SPACE_TYPE_LOCAL:
        return "LOCAL";
      case XR_REFERENCE_SPACE_TYPE_STAGE:
        return "STAGE";
      case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR:
        return "LOCAL_FLOOR";
      default:
        return "REF(" + std::to_string(reference_type) + ")";
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
  enum class GraphicsBindingKind {
    UNKNOWN,
    OPENGL,
    OPENGL_ES,
    VULKAN,
    D3D11,
    D3D12,
    METAL,
  } graphics_binding = GraphicsBindingKind::UNKNOWN;
  std::string graphics_binding_label;
};

// ── Swapchains ──────────────────────────────────────────────────────────────

struct TrackedSwapchainImage {
  XrStructureType type = XR_TYPE_UNKNOWN;
  uint64_t handle_value = 0;
};

struct TrackedPreviewImage {
  bool available = false;
  uint64_t capture_serial = 0;
  uint32_t image_index = 0;
  uint32_t image_array_index = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba8;
};

struct TrackedSwapchain {
  XrSwapchain handle = XR_NULL_HANDLE;
  XrSession session = XR_NULL_HANDLE;
  int64_t format = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t array_size = 0;
  uint32_t face_count = 0;
  uint32_t mip_count = 0;
  uint32_t sample_count = 0;
  XrSwapchainUsageFlags usage_flags = 0;
  XrSwapchainCreateFlags create_flags = 0;
  std::vector<TrackedSwapchainImage> images;
  bool has_latest_acquired_index = false;
  uint32_t latest_acquired_index = 0;
  bool has_latest_released_index = false;
  uint32_t latest_released_index = 0;
  uint64_t release_serial = 0;
  TrackedPreviewImage latest_preview;
  std::unordered_map<uint64_t, TrackedPreviewImage> preview_images;
  uint64_t preview_attempt_count = 0;
  uint64_t preview_success_count = 0;
  uint64_t preview_skip_count = 0;
  std::string preview_status;
  bool preview_logged_success = false;
  std::vector<std::string> preview_logged_failure_statuses;
};

// ── Composition layers ──────────────────────────────────────────────────────

struct TrackedCompositionSubImage {
  XrSwapchain swapchain = XR_NULL_HANDLE;
  int32_t offset_x = 0;
  int32_t offset_y = 0;
  int32_t extent_width = 0;
  int32_t extent_height = 0;
  uint32_t image_array_index = 0;
  bool has_image_index = false;
  uint32_t image_index = 0;
  uint64_t preview_serial = 0;
  bool has_depth = false;
  XrSwapchain depth_swapchain = XR_NULL_HANDLE;
  int32_t depth_offset_x = 0;
  int32_t depth_offset_y = 0;
  int32_t depth_extent_width = 0;
  int32_t depth_extent_height = 0;
  uint32_t depth_image_array_index = 0;
  float min_depth = 0.0f;
  float max_depth = 1.0f;
  float near_z = 0.0f;
  float far_z = 0.0f;
};

struct TrackedCompositionProjectionView {
  XrPosef pose = {{0, 0, 0, 1}, {0, 0, 0}};
  XrFovf fov = {0, 0, 0, 0};
  TrackedCompositionSubImage sub_image;
};

struct TrackedCompositionLayer {
  XrStructureType type = XR_TYPE_UNKNOWN;
  XrCompositionLayerFlags layer_flags = 0;
  XrSpace space = XR_NULL_HANDLE;
  XrEyeVisibility eye_visibility = XR_EYE_VISIBILITY_BOTH;
  XrPosef pose = {{0, 0, 0, 1}, {0, 0, 0}};
  XrExtent2Df size = {0.0f, 0.0f};
  float radius = 0.0f;
  float central_angle = 0.0f;
  float aspect_ratio = 0.0f;
  float scale_x = 0.0f;
  float scale_y = 0.0f;
  float bias_x = 0.0f;
  float bias_y = 0.0f;
  float upper_vertical_angle = 0.0f;
  float lower_vertical_angle = 0.0f;
  uint32_t view_count = 0;
  std::vector<TrackedCompositionProjectionView> projection_views;
  TrackedCompositionSubImage sub_image;
};

struct TrackedCompositionFrame {
  uint64_t frame_number = 0;
  XrTime display_time = 0;
  XrEnvironmentBlendMode environment_blend_mode =
      XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  std::vector<TrackedCompositionLayer> layers;
};

// ── Frame timing ─────────────────────────────────────────────────────────────

struct TrackedFrameState {
  XrTime predicted_display_time = 0;
  XrDuration predicted_display_period = 0;
  XrBool32 should_render = XR_FALSE;
  uint64_t frame_count = 0;

  // Ring buffer of consecutive display-time deltas in milliseconds.
  static constexpr size_t kMaxTimingSamples = 2048;
  std::vector<float> timing_deltas_ms =
      std::vector<float>(kMaxTimingSamples, 0.0f);
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
  int64_t wait_frame_call_ts = 0;   // app calls xrWaitFrame
  int64_t wait_frame_return_ts = 0; // runtime unblocks (returns)
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
  float wait_frame_ms = 0.0f; // how long runtime blocked the app
  float begin_frame_ms = 0.0f;
  float end_frame_ms = 0.0f;
  float app_work_ms =
      0.0f; // begin_frame return → end_frame call (CPU render prep)
  float total_frame_ms =
      0.0f; // wait_frame call → end_frame return (full frame)
  float sync_actions_ms = 0.0f;
  float locate_views_ms = 0.0f;
  float swapchain_acquire_wait_ms =
      0.0f; // sum of all acquire+wait across swapchains
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

  void push_completed_frame() {
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
