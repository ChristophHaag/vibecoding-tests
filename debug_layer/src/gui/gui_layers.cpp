// SPDX-License-Identifier: MIT
// gui/gui_layers.cpp — Composition layer inspection panel.

#include "gui_layers.h"

#include "gui_preview_cache.h"

#include "../instance_data.h"

#include <SDL3/SDL_opengl.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace debug_layer {

namespace {

constexpr uint64_t kCompositionLayerRetentionFrames = 6;

struct DisplayedLayer {
  const TrackedCompositionLayer *layer = nullptr;
  uint64_t source_frame_number = 0;
  size_t source_layer_index = 0;
  uint64_t stale_frame_count = 0;
};

} // namespace

struct PreviewDisplayState {
  bool inspect_mode = false;
  float zoom = 1.0f;
};

static std::unordered_map<PreviewTextureKey, PreviewDisplayState,
                          PreviewTextureKeyHash>
    g_preview_display_state;

static const char *blend_mode_to_str(XrEnvironmentBlendMode mode) {
  switch (mode) {
  case XR_ENVIRONMENT_BLEND_MODE_OPAQUE:
    return "OPAQUE";
  case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE:
    return "ADDITIVE";
  case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND:
    return "ALPHA_BLEND";
  default:
    return "UNKNOWN";
  }
}

static const char *eye_visibility_to_str(XrEyeVisibility eye_visibility) {
  switch (eye_visibility) {
  case XR_EYE_VISIBILITY_BOTH:
    return "BOTH";
  case XR_EYE_VISIBILITY_LEFT:
    return "LEFT";
  case XR_EYE_VISIBILITY_RIGHT:
    return "RIGHT";
  default:
    return "UNKNOWN";
  }
}

static const char *layer_type_to_str(XrStructureType type) {
  switch (type) {
  case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
    return "Projection";
  case XR_TYPE_COMPOSITION_LAYER_QUAD:
    return "Quad";
  case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
    return "Cylinder";
  case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
    return "Equirect";
  case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
    return "Equirect2";
  case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
    return "Cube";
  default:
    return "Unknown";
  }
}

static void format_handle(char *buffer, size_t buffer_size, uint64_t handle) {
  std::snprintf(buffer, buffer_size, "0x%llx", (unsigned long long)handle);
}

static float preview_avg_ms(double total_ms, uint64_t sample_count) {
  return sample_count == 0 ? 0.0f : (float)(total_ms / (double)sample_count);
}

static const char *space_label(const InstanceData *data, XrSpace space,
                               char *fallback, size_t fallback_size) {
  if (space == XR_NULL_HANDLE)
    return "(none)";

  auto it = data->spaces.find(space);
  if (it != data->spaces.end())
    return it->second.label().c_str();

  format_handle(fallback, fallback_size, (uint64_t)space);
  return fallback;
}

static std::string sub_image_identity_key(const TrackedCompositionSubImage &sub) {
  std::ostringstream oss;
  oss << std::hex << (uint64_t)sub.swapchain << std::dec << ':'
      << sub.image_array_index << ':' << sub.offset_x << ':' << sub.offset_y
      << ':' << sub.extent_width << ':' << sub.extent_height;
  return oss.str();
}

static std::string layer_identity_key(size_t layer_index,
                                      const TrackedCompositionLayer &layer) {
  std::ostringstream oss;
  oss << layer_index << ':' << layer.type << ':' << std::hex
      << (uint64_t)layer.space << std::dec << ':' << layer.eye_visibility;
  if (layer.type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
    oss << ":views=" << layer.projection_views.size();
    for (const auto &view : layer.projection_views)
      oss << ':' << sub_image_identity_key(view.sub_image);
  } else {
    oss << ':' << sub_image_identity_key(layer.sub_image);
  }
  return oss.str();
}

static std::vector<DisplayedLayer>
collect_display_layers(const std::deque<TrackedCompositionFrame> &frames) {
  std::vector<DisplayedLayer> layers;
  if (frames.empty())
    return layers;

  const uint64_t newest_frame_number = frames.back().frame_number;
  std::unordered_set<std::string> seen_keys;
  for (auto frame_it = frames.rbegin(); frame_it != frames.rend(); ++frame_it) {
    uint64_t stale_frame_count =
        newest_frame_number >= frame_it->frame_number
            ? newest_frame_number - frame_it->frame_number
            : 0;
    if (stale_frame_count > kCompositionLayerRetentionFrames)
      break;

    for (size_t layer_index = 0; layer_index < frame_it->layers.size();
         ++layer_index) {
      const TrackedCompositionLayer &layer = frame_it->layers[layer_index];
      std::string key = layer_identity_key(layer_index, layer);
      if (!seen_keys.insert(key).second)
        continue;

      DisplayedLayer displayed;
      displayed.layer = &layer;
      displayed.source_frame_number = frame_it->frame_number;
      displayed.source_layer_index = layer_index;
      displayed.stale_frame_count = stale_frame_count;
      layers.push_back(displayed);
    }
  }

  return layers;
}

static const char *preview_note_for_swapchain(const InstanceData *data,
                                              const TrackedSwapchain &swapchain) {
  if (!swapchain.preview_status.empty())
    return swapchain.preview_status.c_str();

  auto session_it = data->sessions.find(swapchain.session);
  if (session_it == data->sessions.end() ||
      session_it->second.graphics_binding !=
          TrackedSession::GraphicsBindingKind::OPENGL) {
    return "Preview capture is currently implemented only for OpenGL sessions.";
  }

  if ((swapchain.usage_flags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) == 0)
    return "Preview capture is disabled for non-color swapchains.";

  if (swapchain.face_count != 1)
    return "Preview capture does not yet support cube or multi-face swapchains.";

  if (swapchain.array_size > 1)
    return "Preview currently shows array layer 0 only.";

  return "Waiting for the next throttled OpenGL preview capture.";
}

static void draw_sub_image_overlay(ImDrawList *draw_list, ImVec2 min,
                                   ImVec2 canvas_size, int32_t full_width,
                                   int32_t full_height,
                                   const TrackedCompositionSubImage &sub_image) {
  float x0 =
      min.x + ((float)sub_image.offset_x / (float)full_width) * canvas_size.x;
  float y0 =
      min.y + ((float)sub_image.offset_y / (float)full_height) * canvas_size.y;
  float x1 =
      x0 + ((float)sub_image.extent_width / (float)full_width) * canvas_size.x;
  float y1 = y0 + ((float)sub_image.extent_height / (float)full_height) *
                      canvas_size.y;
  draw_list->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                           IM_COL32(70, 170, 255, 72), 2.0f);
  draw_list->AddRect(ImVec2(x0, y0), ImVec2(x1, y1),
                     IM_COL32(70, 170, 255, 255), 2.0f, 0, 2.0f);

  char label[96];
  std::snprintf(label, sizeof(label), "%dx%d -> (%d,%d %dx%d)", full_width,
                full_height, sub_image.offset_x, sub_image.offset_y,
                sub_image.extent_width, sub_image.extent_height);
  draw_list->AddText(ImVec2(min.x + 6.0f, min.y + 6.0f),
                     IM_COL32(230, 230, 235, 255), label);
}

static void draw_image_rect_visualization(
    const char *id, int32_t full_width, int32_t full_height,
    const TrackedCompositionSubImage &sub_image, GLuint preview_texture) {
  if (full_width <= 0 || full_height <= 0)
    return;

  const float max_width = 320.0f;
  const float max_height = 220.0f;
  float aspect = (float)full_width / (float)full_height;
  ImVec2 canvas_size(max_width, max_width / std::max(aspect, 0.01f));
  if (canvas_size.y > max_height) {
    canvas_size.y = max_height;
    canvas_size.x = max_height * aspect;
  }

  ImGui::InvisibleButton(id, canvas_size);
  ImDrawList *draw_list = ImGui::GetWindowDrawList();
  ImVec2 min = ImGui::GetItemRectMin();
  ImVec2 max = ImGui::GetItemRectMax();

  if (preview_texture != 0) {
    draw_list->AddImage((ImTextureID)(intptr_t)preview_texture, min, max,
                        ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
    draw_list->AddRectFilled(min, max, IM_COL32(16, 18, 22, 70), 4.0f);
  } else {
    draw_list->AddRectFilled(min, max, IM_COL32(28, 30, 34, 255), 4.0f);
  }
  draw_list->AddRect(min, max, IM_COL32(160, 165, 175, 255), 4.0f, 0, 1.5f);
  draw_sub_image_overlay(draw_list, min, canvas_size, full_width, full_height,
                         sub_image);
}

static void render_preview_inspector(
    const char *id, const PreviewTextureKey &cache_key,
    const TrackedPreviewImage &preview, GLuint preview_texture,
    int32_t full_width, int32_t full_height,
    const TrackedCompositionSubImage &sub_image) {
  PreviewDisplayState &display = g_preview_display_state[cache_key];

  if (!display.inspect_mode) {
    draw_image_rect_visualization(id, full_width, full_height, sub_image,
                                  preview_texture);
    if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      display.inspect_mode = true;
      display.zoom = 1.0f;
    }
    return;
  }

  ImGui::PushID((void *)(uintptr_t)cache_key.swapchain);
  ImGui::PushID((int)cache_key.image_index);
  ImGui::PushID((int)cache_key.image_array_index);
  if (ImGui::SmallButton("Fit View")) {
    display.inspect_mode = false;
    display.zoom = 1.0f;
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(150.0f);
  ImGui::SliderFloat("Zoom", &display.zoom, 0.25f, 16.0f, "%.2fx",
                     ImGuiSliderFlags_Logarithmic);
  ImGui::SameLine();
  if (ImGui::SmallButton("Reset"))
    display.zoom = 1.0f;
  ImGui::TextDisabled("Mouse wheel zooms. Click the image to return to fit view.");

  ImVec2 image_size(preview.width * display.zoom, preview.height * display.zoom);
  float child_height = std::min(420.0f, std::max(160.0f, image_size.y + 12.0f));
  ImGui::BeginChild("preview_inspect", ImVec2(0.0f, child_height),
                    ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::Image((ImTextureID)(intptr_t)preview_texture, image_size);

  ImDrawList *draw_list = ImGui::GetWindowDrawList();
  ImVec2 min = ImGui::GetItemRectMin();
  draw_list->AddRect(min, ImVec2(min.x + image_size.x, min.y + image_size.y),
                     IM_COL32(160, 165, 175, 255), 0.0f, 0, 1.0f);
  draw_sub_image_overlay(draw_list, min, image_size, full_width, full_height,
                         sub_image);

  if (ImGui::IsItemHovered()) {
    float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f)
      display.zoom = std::clamp(display.zoom * (wheel > 0.0f ? 1.2f : 1.0f / 1.2f),
                                0.25f, 16.0f);

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      display.inspect_mode = false;
      display.zoom = 1.0f;
    }

    ImVec2 mouse = ImGui::GetIO().MousePos;
    int pixel_x = (int)std::floor((mouse.x - min.x) / display.zoom);
    int pixel_y = (int)std::floor((mouse.y - min.y) / display.zoom);
    if (pixel_x >= 0 && pixel_y >= 0 && pixel_x < (int)preview.width &&
        pixel_y < (int)preview.height) {
      size_t pixel_index =
          ((size_t)pixel_y * (size_t)preview.width + (size_t)pixel_x) * 4;
      if (pixel_index + 3 < preview.rgba8.size()) {
        ImGui::BeginTooltip();
        ImGui::Text("Pixel (%d, %d)", pixel_x, pixel_y);
        ImGui::Text("RGBA = (%u, %u, %u, %u)", preview.rgba8[pixel_index + 0],
                    preview.rgba8[pixel_index + 1],
                    preview.rgba8[pixel_index + 2],
                    preview.rgba8[pixel_index + 3]);
        ImGui::Text("Zoom %.2fx", display.zoom);
        ImGui::EndTooltip();
      }
    }
  }

  ImGui::EndChild();
  ImGui::PopID();
  ImGui::PopID();
  ImGui::PopID();
}

static void render_sub_image(const InstanceData *data, const char *heading,
                             const TrackedCompositionSubImage &sub_image,
                             int tree_suffix) {
  if (sub_image.swapchain == XR_NULL_HANDLE) {
    ImGui::TextDisabled("%s: no swapchain", heading);
    return;
  }

  char swapchain_buf[32];
  format_handle(swapchain_buf, sizeof(swapchain_buf),
                (uint64_t)sub_image.swapchain);

  const TrackedSwapchain *swapchain = nullptr;
  auto it = data->swapchains.find(sub_image.swapchain);
  if (it != data->swapchains.end())
    swapchain = &it->second;

  ImGui::SeparatorText(heading);
  ImGui::Text("Swapchain: %s", swapchain_buf);
  if (sub_image.has_image_index)
    ImGui::Text("Image index: %u", sub_image.image_index);
  else
    ImGui::TextDisabled("Image index: unavailable");

  ImGui::Text("Image rect: offset=(%d,%d) extent=(%d,%d)", sub_image.offset_x,
              sub_image.offset_y, sub_image.extent_width,
              sub_image.extent_height);
  ImGui::Text("Array index: %u", sub_image.image_array_index);

  if (swapchain != nullptr) {
    PreviewLookupResult preview_lookup =
        gui_lookup_preview_for_sub_image(*swapchain, sub_image);
    const TrackedPreviewImage *preview = preview_lookup.preview;
    bool has_matching_preview = preview != nullptr && preview->available;
    PreviewTextureKey preview_texture_key = preview_lookup.texture_key;
    GLuint preview_texture = 0;
    if (has_matching_preview)
      preview_texture = gui_ensure_preview_texture(preview_texture_key, *preview);

    ImGui::Text("Swapchain extent: %ux%u", swapchain->width, swapchain->height);
    ImGui::Text("Format: %#llx  Samples: %u  Mips: %u",
                (unsigned long long)swapchain->format, swapchain->sample_count,
                swapchain->mip_count);

    if (sub_image.has_image_index &&
        sub_image.image_index < swapchain->images.size()) {
      char image_handle_buf[32];
      format_handle(image_handle_buf, sizeof(image_handle_buf),
                    swapchain->images[sub_image.image_index].handle_value);
      ImGui::Text("Native image handle: %s", image_handle_buf);
    }

    char viz_id[32];
    std::snprintf(viz_id, sizeof(viz_id), "##rect_%s_%d", heading, tree_suffix);
    if (has_matching_preview) {
      render_preview_inspector(viz_id, preview_texture_key, *preview,
                               preview_texture, (int32_t)swapchain->width,
                               (int32_t)swapchain->height, sub_image);
    } else {
      draw_image_rect_visualization(viz_id, (int32_t)swapchain->width,
                                    (int32_t)swapchain->height, sub_image,
                                    preview_texture);
    }

    ImGui::TextDisabled("%s", has_matching_preview
                                 ? "Click preview to inspect at actual pixels."
                                 : "Preview unavailable for this sub-image.");

    if (has_matching_preview) {
      ImGui::Text("Preview: %ux%u  serial=%llu  array=%u",
                  preview->width, preview->height,
                  (unsigned long long)preview->capture_serial,
                  preview->image_array_index);
    } else {
      ImGui::TextDisabled("Preview: %s", preview_note_for_swapchain(data, *swapchain));
    }

    ImGui::Text("Preview stats: attempts=%llu success=%llu skipped=%llu",
                (unsigned long long)swapchain->preview_attempt_count,
                (unsigned long long)swapchain->preview_success_count,
                (unsigned long long)swapchain->preview_skip_count);
    ImGui::Text(
      "Preview timing: app-thread %.3f / %.3f / %.3f ms (last/avg/max)",
      swapchain->preview_app_thread_ms_last,
      preview_avg_ms(swapchain->preview_app_thread_ms_total,
               swapchain->preview_app_thread_sample_count),
      swapchain->preview_app_thread_ms_max);
    ImGui::Text(
      "Preview finalize: %.3f / %.3f / %.3f ms   latency: %.3f / %.3f / %.3f ms",
      swapchain->preview_finalize_ms_last,
      preview_avg_ms(swapchain->preview_finalize_ms_total,
               swapchain->preview_finalize_sample_count),
      swapchain->preview_finalize_ms_max,
      swapchain->preview_ready_latency_ms_last,
      preview_avg_ms(swapchain->preview_ready_latency_ms_total,
               swapchain->preview_ready_latency_sample_count),
      swapchain->preview_ready_latency_ms_max);
    ImGui::Text("Preview in-flight: %u current  %u peak",
          swapchain->preview_inflight_count,
          swapchain->preview_inflight_peak);
  } else {
    ImGui::TextDisabled("Swapchain metadata not tracked yet");
  }

  if (sub_image.has_depth) {
    char depth_buf[32];
    format_handle(depth_buf, sizeof(depth_buf),
                  (uint64_t)sub_image.depth_swapchain);
    ImGui::Text("Depth swapchain: %s", depth_buf);
    ImGui::Text("Depth rect: offset=(%d,%d) extent=(%d,%d) array=%u",
                sub_image.depth_offset_x, sub_image.depth_offset_y,
                sub_image.depth_extent_width, sub_image.depth_extent_height,
                sub_image.depth_image_array_index);
    ImGui::Text("Depth range: min=%.3f max=%.3f near=%.3f far=%.3f",
                sub_image.min_depth, sub_image.max_depth, sub_image.near_z,
                sub_image.far_z);
  }
}

void gui_render_layers_panel(InstanceData *data) {
  std::shared_lock lock(data->state_mutex);

  std::unordered_set<uint64_t> live_swapchains;
  live_swapchains.reserve(data->swapchains.size());
  for (const auto &[swapchain_handle, swapchain] : data->swapchains)
    live_swapchains.insert((uint64_t)swapchain_handle);
  gui_prune_preview_textures(live_swapchains);

  for (auto it = g_preview_display_state.begin();
       it != g_preview_display_state.end();) {
    if (data->swapchains.find((XrSwapchain)it->first.swapchain) ==
        data->swapchains.end())
      it = g_preview_display_state.erase(it);
    else
      ++it;
  }

  ImGui::Begin("Composition Layers");

  if (data->composition_frames.empty()) {
    ImGui::TextDisabled("Waiting for xrEndFrame submissions...");
    ImGui::End();
    return;
  }

  const TrackedCompositionFrame &frame = data->composition_frames.back();
  std::vector<DisplayedLayer> displayed_layers =
      collect_display_layers(data->composition_frames);

  ImGui::Text("Frame #%llu", (unsigned long long)frame.frame_number);
  ImGui::Text("Display time: %lld", (long long)frame.display_time);
  ImGui::Text("Blend mode: %s",
              blend_mode_to_str(frame.environment_blend_mode));
  ImGui::Text("Submitted layers this frame: %zu", frame.layers.size());
  ImGui::Text("Displayed layers: %zu  Retention window: %llu frames",
              displayed_layers.size(),
              (unsigned long long)kCompositionLayerRetentionFrames);

  if (!data->sessions.empty()) {
    ImGui::SeparatorText("Session");
    for (const auto &[session_handle, session] : data->sessions) {
      char handle_buf[32];
      format_handle(handle_buf, sizeof(handle_buf), (uint64_t)session_handle);
      ImGui::Text("%s  Graphics: %s", handle_buf,
                  session.graphics_binding_label.empty()
                      ? "Unknown"
                      : session.graphics_binding_label.c_str());
    }
  }

    for (size_t layer_list_index = 0; layer_list_index < displayed_layers.size();
      ++layer_list_index) {
      const DisplayedLayer &displayed = displayed_layers[layer_list_index];
      const TrackedCompositionLayer &layer = *displayed.layer;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen;
      if (ImGui::TreeNodeEx(
        (void *)(uintptr_t)(layer_list_index + 1), flags,
        displayed.stale_frame_count == 0
         ? "Layer %zu: %s"
         : "Layer %zu: %s  [stale %llu frame%s]",
        displayed.source_layer_index, layer_type_to_str(layer.type),
        (unsigned long long)displayed.stale_frame_count,
        displayed.stale_frame_count == 1 ? "" : "s")) {
      char space_buf[32];
      ImGui::Text("Space: %s",
                  space_label(data, layer.space, space_buf, sizeof(space_buf)));
      ImGui::Text("Layer flags: %#llx", (unsigned long long)layer.layer_flags);
      ImGui::Text("Eye visibility: %s",
                  eye_visibility_to_str(layer.eye_visibility));
     ImGui::Text("Last seen in frame: %llu",
           (unsigned long long)displayed.source_frame_number);

      switch (layer.type) {
      case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
        ImGui::Text("Views: %u", layer.view_count);
        for (size_t view_index = 0; view_index < layer.projection_views.size();
             ++view_index) {
          const auto &view = layer.projection_views[view_index];
          ImGui::SeparatorText(view_index == 0 ? "View 0" : "View 1");
          ImGui::Text("Pose position: (%.3f, %.3f, %.3f)", view.pose.position.x,
                      view.pose.position.y, view.pose.position.z);
          ImGui::Text("FOV: L=%.3f R=%.3f U=%.3f D=%.3f", view.fov.angleLeft,
                      view.fov.angleRight, view.fov.angleUp,
                      view.fov.angleDown);
          render_sub_image(data, "Color sub-image", view.sub_image,
                           (int)(layer_list_index * 8 + view_index));
        }
        break;
      case XR_TYPE_COMPOSITION_LAYER_QUAD:
        ImGui::Text("Pose position: (%.3f, %.3f, %.3f)", layer.pose.position.x,
                    layer.pose.position.y, layer.pose.position.z);
        ImGui::Text("Size: %.3f x %.3f", layer.size.width, layer.size.height);
        render_sub_image(data, "Color sub-image", layer.sub_image,
                         (int)layer_list_index);
        break;
      case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
        ImGui::Text("Pose position: (%.3f, %.3f, %.3f)", layer.pose.position.x,
                    layer.pose.position.y, layer.pose.position.z);
        ImGui::Text("Radius: %.3f  Central angle: %.3f  Aspect ratio: %.3f",
                    layer.radius, layer.central_angle, layer.aspect_ratio);
        render_sub_image(data, "Color sub-image", layer.sub_image,
                         (int)layer_list_index);
        break;
      case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
        ImGui::Text("Pose position: (%.3f, %.3f, %.3f)", layer.pose.position.x,
                    layer.pose.position.y, layer.pose.position.z);
        ImGui::Text("Radius: %.3f  Scale=(%.3f, %.3f)  Bias=(%.3f, %.3f)",
                    layer.radius, layer.scale_x, layer.scale_y, layer.bias_x,
                    layer.bias_y);
        render_sub_image(data, "Color sub-image", layer.sub_image,
                         (int)layer_list_index);
        break;
      case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
        ImGui::Text("Pose position: (%.3f, %.3f, %.3f)", layer.pose.position.x,
                    layer.pose.position.y, layer.pose.position.z);
        ImGui::Text(
            "Radius: %.3f  Horizontal angle: %.3f  Upper/Lower: %.3f / %.3f",
            layer.radius, layer.central_angle, layer.upper_vertical_angle,
            layer.lower_vertical_angle);
        render_sub_image(data, "Color sub-image", layer.sub_image,
                 (int)layer_list_index);
        break;
      case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
        ImGui::Text("Pose position: (%.3f, %.3f, %.3f)", layer.pose.position.x,
                    layer.pose.position.y, layer.pose.position.z);
        render_sub_image(data, "Cube sub-image", layer.sub_image,
                         (int)layer_list_index);
        break;
      default:
        ImGui::TextDisabled(
            "Detailed inspection not implemented for this layer type yet.");
        break;
      }

      ImGui::TreePop();
    }
  }

  ImGui::End();
}

} // namespace debug_layer