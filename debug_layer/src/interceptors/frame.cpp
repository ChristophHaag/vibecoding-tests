// SPDX-License-Identifier: MIT
// interceptors/frame.cpp — xrWaitFrame, xrBeginFrame, xrEndFrame, swapchain
// ops.

#include "../dispatch.h"
#include "../instance_data.h"

#include <EGL/egl.h>
#include <GL/glx.h>
#include <SDL3/SDL_opengl.h>

#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace debug_layer {

// CLOCK_MONOTONIC nanoseconds — same timebase as XrTime on Linux/Monado.
static int64_t now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static float ns_to_ms(int64_t a, int64_t b) { return (float)((b - a) * 1e-6); }

struct PreviewCaptureConfig {
  int capture_interval = 2;
  int max_edge = 320;
};

static int parse_env_int(const char *name, int fallback, int min_value,
                         int max_value) {
  const char *env = std::getenv(name);
  if (env == nullptr || env[0] == '\0')
    return fallback;

  int parsed = std::atoi(env);
  return std::clamp(parsed, min_value, max_value);
}

static PreviewCaptureConfig get_preview_capture_config() {
  static PreviewCaptureConfig config = [] {
    PreviewCaptureConfig c;
    c.capture_interval =
        parse_env_int("XR_DEBUG_GUI_GL_PREVIEW_INTERVAL", 2, 0, 120);
    c.max_edge = parse_env_int("XR_DEBUG_GUI_GL_PREVIEW_MAX_EDGE", 320, 32,
                               2048);
    return c;
  }();
  return config;
}

static uint64_t make_preview_key(uint32_t image_index,
                                 uint32_t image_array_index) {
  return ((uint64_t)image_array_index << 32) | (uint64_t)image_index;
}

static int get_preview_log_level() {
  static int level = parse_env_int("XR_DEBUG_GUI_GL_PREVIEW_LOG", 0, 0, 2);
  return level;
}

static struct {
  bool loaded = false;
  PFNGLGENFRAMEBUFFERSPROC GenFramebuffers = nullptr;
  PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = nullptr;
  PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D = nullptr;
  PFNGLFRAMEBUFFERTEXTURELAYERPROC FramebufferTextureLayer = nullptr;
  PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers = nullptr;
  PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus = nullptr;
  PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer = nullptr;
} preview_gl;

static void *get_preview_gl_proc_address(const char *name) {
  void *proc = nullptr;

#if defined(XR_USE_PLATFORM_EGL) || defined(__linux__)
  proc = (void *)eglGetProcAddress(name);
  if (proc != nullptr)
    return proc;
#endif

#if defined(__linux__)
  proc = (void *)glXGetProcAddressARB((const GLubyte *)name);
#endif

  return proc;
}

static bool load_preview_gl_functions() {
  if (preview_gl.loaded) {
    return preview_gl.GenFramebuffers != nullptr &&
           preview_gl.BindFramebuffer != nullptr &&
           preview_gl.FramebufferTexture2D != nullptr &&
           preview_gl.FramebufferTextureLayer != nullptr &&
           preview_gl.DeleteFramebuffers != nullptr &&
           preview_gl.CheckFramebufferStatus != nullptr &&
           preview_gl.BlitFramebuffer != nullptr;
  }

#define LOAD(name)                                                             \
  preview_gl.name =                                                           \
      (decltype(preview_gl.name))get_preview_gl_proc_address("gl" #name)
  LOAD(GenFramebuffers);
  LOAD(BindFramebuffer);
  LOAD(FramebufferTexture2D);
  LOAD(FramebufferTextureLayer);
  LOAD(DeleteFramebuffers);
  LOAD(CheckFramebufferStatus);
  LOAD(BlitFramebuffer);
#undef LOAD

  preview_gl.loaded = true;
  return preview_gl.GenFramebuffers != nullptr &&
         preview_gl.BindFramebuffer != nullptr &&
         preview_gl.FramebufferTexture2D != nullptr &&
         preview_gl.FramebufferTextureLayer != nullptr &&
         preview_gl.DeleteFramebuffers != nullptr &&
         preview_gl.CheckFramebufferStatus != nullptr &&
         preview_gl.BlitFramebuffer != nullptr;
}

enum class PreviewAttachmentKind {
  Unsupported,
  Texture2D,
  Texture2DArrayLayer0,
};

static PreviewAttachmentKind
get_preview_attachment_kind(const TrackedSwapchain &swapchain) {
  if (swapchain.face_count != 1)
    return PreviewAttachmentKind::Unsupported;
  if (swapchain.array_size > 1)
    return PreviewAttachmentKind::Texture2DArrayLayer0;
  return PreviewAttachmentKind::Texture2D;
}

static const char *preview_capture_skip_reason(const TrackedSession *session,
                                               const TrackedSwapchain &swapchain) {
  if (session == nullptr ||
      session->graphics_binding != TrackedSession::GraphicsBindingKind::OPENGL)
    return "Session graphics binding is not classified as desktop OpenGL";

  if ((swapchain.usage_flags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) == 0)
    return "Swapchain is not a color attachment";

  if (!swapchain.has_latest_acquired_index ||
      swapchain.latest_acquired_index >= swapchain.images.size())
    return "No acquired swapchain image is available yet";

  if (swapchain.images[swapchain.latest_acquired_index].type !=
      XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR)
    return "Swapchain image type is not XrSwapchainImageOpenGLKHR";

  if (get_preview_attachment_kind(swapchain) ==
      PreviewAttachmentKind::Unsupported)
    return "Cube and multi-face OpenGL swapchains are not supported yet";

  PreviewCaptureConfig config = get_preview_capture_config();
  if (config.capture_interval <= 0)
    return "Preview capture disabled by XR_DEBUG_GUI_GL_PREVIEW_INTERVAL=0";

  uint64_t pending_serial = swapchain.release_serial + 1;
  if (pending_serial != 1 &&
      pending_serial % (uint64_t)config.capture_interval != 0)
    return "Waiting for the configured preview capture interval";

  return nullptr;
}

static void flip_rgba_rows(std::vector<uint8_t> &rgba, uint32_t width,
                           uint32_t height) {
  if (height < 2 || width == 0)
    return;

  const size_t row_bytes = (size_t)width * 4;
  std::vector<uint8_t> temp(row_bytes);
  for (uint32_t top = 0, bottom = height - 1; top < bottom; ++top, --bottom) {
    uint8_t *top_row = rgba.data() + (size_t)top * row_bytes;
    uint8_t *bottom_row = rgba.data() + (size_t)bottom * row_bytes;
    std::copy(top_row, top_row + row_bytes, temp.data());
    std::copy(bottom_row, bottom_row + row_bytes, top_row);
    std::copy(temp.data(), temp.data() + row_bytes, bottom_row);
  }
}

static bool capture_opengl_preview(const TrackedSwapchain &swapchain,
                                   TrackedPreviewImage &preview,
                                   std::string &status) {
  if (!load_preview_gl_functions())
  {
    status = "OpenGL preview helpers are unavailable on this thread";
    return false;
  }

  PreviewCaptureConfig config = get_preview_capture_config();
  if (swapchain.width == 0 || swapchain.height == 0)
  {
    status = "Swapchain extent is zero";
    return false;
  }

  uint32_t preview_width = swapchain.width;
  uint32_t preview_height = swapchain.height;
  uint32_t longest_edge = std::max(preview_width, preview_height);
  if (longest_edge > (uint32_t)config.max_edge) {
    float scale = (float)config.max_edge / (float)longest_edge;
    preview_width = std::max(1u, (uint32_t)(preview_width * scale));
    preview_height = std::max(1u, (uint32_t)(preview_height * scale));
  }

  GLuint src_fbo = 0;
  GLuint dst_fbo = 0;
  GLuint dst_texture = 0;
  GLint prev_read_fbo = 0;
  GLint prev_draw_fbo = 0;
  GLint prev_texture_2d = 0;
  GLint prev_read_buffer = 0;
  GLint prev_draw_buffer = 0;
  GLint prev_pack_alignment = 0;
  bool success = false;

  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_texture_2d);
  glGetIntegerv(GL_READ_BUFFER, &prev_read_buffer);
  glGetIntegerv(GL_DRAW_BUFFER, &prev_draw_buffer);
  glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack_alignment);

  glGenTextures(1, &dst_texture);
  glBindTexture(GL_TEXTURE_2D, dst_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)preview_width,
               (GLsizei)preview_height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               nullptr);

  preview_gl.GenFramebuffers(1, &src_fbo);
  preview_gl.GenFramebuffers(1, &dst_fbo);

  preview_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
  switch (get_preview_attachment_kind(swapchain)) {
  case PreviewAttachmentKind::Texture2D:
    preview_gl.FramebufferTexture2D(
        GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
        (GLuint)swapchain.images[swapchain.latest_acquired_index].handle_value, 0);
    break;
  case PreviewAttachmentKind::Texture2DArrayLayer0:
    preview_gl.FramebufferTextureLayer(
        GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        (GLuint)swapchain.images[swapchain.latest_acquired_index].handle_value, 0, 0);
    break;
  case PreviewAttachmentKind::Unsupported:
    break;
  }

  preview_gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
  preview_gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_TEXTURE_2D, dst_texture, 0);

    GLenum read_status = preview_gl.CheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    GLenum draw_status = preview_gl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (read_status == GL_FRAMEBUFFER_COMPLETE &&
      draw_status == GL_FRAMEBUFFER_COMPLETE) {
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    preview_gl.BlitFramebuffer(0, 0, (GLint)swapchain.width,
                               (GLint)swapchain.height, 0, 0,
                               (GLint)preview_width, (GLint)preview_height,
                               GL_COLOR_BUFFER_BIT, GL_LINEAR);

    preview_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, dst_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    preview.rgba8.resize((size_t)preview_width * (size_t)preview_height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)preview_width, (GLsizei)preview_height,
                 GL_RGBA, GL_UNSIGNED_BYTE, preview.rgba8.data());
    flip_rgba_rows(preview.rgba8, preview_width, preview_height);

    preview.available = true;
    preview.capture_serial = swapchain.release_serial + 1;
    preview.image_index = swapchain.latest_acquired_index;
    preview.image_array_index = 0;
    preview.width = preview_width;
    preview.height = preview_height;
    status = swapchain.array_size > 1
                 ? "Captured preview from array layer 0"
                 : "Captured preview";
    success = true;
  } else {
    std::ostringstream oss;
    oss << "Framebuffer incomplete (read=0x" << std::hex << read_status
        << ", draw=0x" << draw_status << ")";
    status = oss.str();
  }

  preview_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read_fbo);
  preview_gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw_fbo);
  glReadBuffer(prev_read_buffer);
  glDrawBuffer(prev_draw_buffer);
  glPixelStorei(GL_PACK_ALIGNMENT, prev_pack_alignment);
  glBindTexture(GL_TEXTURE_2D, (GLuint)prev_texture_2d);

  if (dst_fbo != 0)
    preview_gl.DeleteFramebuffers(1, &dst_fbo);
  if (src_fbo != 0)
    preview_gl.DeleteFramebuffers(1, &src_fbo);
  if (dst_texture != 0)
    glDeleteTextures(1, &dst_texture);

  return success;
}

static TrackedSwapchainImage
track_swapchain_image(const XrSwapchainImageBaseHeader &image) {
  TrackedSwapchainImage tracked;
  tracked.type = image.type;

  if (image.type == XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR) {
    auto &gl_image = reinterpret_cast<const XrSwapchainImageOpenGLKHR &>(image);
    tracked.handle_value = (uint64_t)gl_image.image;
  }

  return tracked;
}

static void
maybe_attach_known_image_index(const TrackedSwapchain &swapchain,
                               TrackedCompositionSubImage &sub_image) {
  if (sub_image.swapchain == XR_NULL_HANDLE)
    return;

  if (swapchain.has_latest_released_index) {
    sub_image.has_image_index = true;
    sub_image.image_index = swapchain.latest_released_index;
    auto preview_it = swapchain.preview_images.find(
        make_preview_key(sub_image.image_index, sub_image.image_array_index));
    if (preview_it != swapchain.preview_images.end())
      sub_image.preview_serial = preview_it->second.capture_serial;
    else
      sub_image.preview_serial = swapchain.latest_preview.capture_serial;
  } else if (swapchain.has_latest_acquired_index) {
    sub_image.has_image_index = true;
    sub_image.image_index = swapchain.latest_acquired_index;
  }
}

static TrackedCompositionSubImage
track_sub_image(const InstanceData *data,
                const XrSwapchainSubImage &sub_image) {
  TrackedCompositionSubImage tracked;
  tracked.swapchain = sub_image.swapchain;
  tracked.offset_x = sub_image.imageRect.offset.x;
  tracked.offset_y = sub_image.imageRect.offset.y;
  tracked.extent_width = sub_image.imageRect.extent.width;
  tracked.extent_height = sub_image.imageRect.extent.height;
  tracked.image_array_index = sub_image.imageArrayIndex;

  auto it = data->swapchains.find(sub_image.swapchain);
  if (it != data->swapchains.end())
    maybe_attach_known_image_index(it->second, tracked);

  return tracked;
}

static void fill_depth_from_next_chain(const InstanceData *data,
                                       const void *next,
                                       TrackedCompositionSubImage &tracked) {
  const XrBaseInStructure *current =
      reinterpret_cast<const XrBaseInStructure *>(next);
  while (current != nullptr) {
    if (current->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
      auto *depth =
          reinterpret_cast<const XrCompositionLayerDepthInfoKHR *>(current);
      tracked.has_depth = true;
      tracked.depth_swapchain = depth->subImage.swapchain;
      tracked.depth_offset_x = depth->subImage.imageRect.offset.x;
      tracked.depth_offset_y = depth->subImage.imageRect.offset.y;
      tracked.depth_extent_width = depth->subImage.imageRect.extent.width;
      tracked.depth_extent_height = depth->subImage.imageRect.extent.height;
      tracked.depth_image_array_index = depth->subImage.imageArrayIndex;
      tracked.min_depth = depth->minDepth;
      tracked.max_depth = depth->maxDepth;
      tracked.near_z = depth->nearZ;
      tracked.far_z = depth->farZ;

      auto it = data->swapchains.find(depth->subImage.swapchain);
      if (it != data->swapchains.end())
        maybe_attach_known_image_index(it->second, tracked);
      return;
    }
    current = current->next;
  }
}

static TrackedCompositionLayer
track_layer(const InstanceData *data,
            const XrCompositionLayerBaseHeader &layer) {
  TrackedCompositionLayer tracked;
  tracked.type = layer.type;
  tracked.layer_flags = layer.layerFlags;
  tracked.space = layer.space;

  switch (layer.type) {
  case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
    auto &projection =
        reinterpret_cast<const XrCompositionLayerProjection &>(layer);
    tracked.view_count = projection.viewCount;
    tracked.projection_views.reserve(projection.viewCount);
    for (uint32_t view_index = 0; view_index < projection.viewCount;
         ++view_index) {
      TrackedCompositionProjectionView view;
      view.pose = projection.views[view_index].pose;
      view.fov = projection.views[view_index].fov;
      view.sub_image =
          track_sub_image(data, projection.views[view_index].subImage);
      fill_depth_from_next_chain(data, projection.views[view_index].next,
                                 view.sub_image);
      tracked.projection_views.push_back(view);
    }
    break;
  }
  case XR_TYPE_COMPOSITION_LAYER_QUAD: {
    auto &quad = reinterpret_cast<const XrCompositionLayerQuad &>(layer);
    tracked.eye_visibility = quad.eyeVisibility;
    tracked.pose = quad.pose;
    tracked.size = quad.size;
    tracked.sub_image = track_sub_image(data, quad.subImage);
    fill_depth_from_next_chain(data, quad.next, tracked.sub_image);
    break;
  }
  case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR: {
    auto &cylinder =
        reinterpret_cast<const XrCompositionLayerCylinderKHR &>(layer);
    tracked.eye_visibility = cylinder.eyeVisibility;
    tracked.pose = cylinder.pose;
    tracked.radius = cylinder.radius;
    tracked.central_angle = cylinder.centralAngle;
    tracked.aspect_ratio = cylinder.aspectRatio;
    tracked.sub_image = track_sub_image(data, cylinder.subImage);
    fill_depth_from_next_chain(data, cylinder.next, tracked.sub_image);
    break;
  }
  case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR: {
    auto &equirect =
        reinterpret_cast<const XrCompositionLayerEquirectKHR &>(layer);
    tracked.eye_visibility = equirect.eyeVisibility;
    tracked.pose = equirect.pose;
    tracked.radius = equirect.radius;
    tracked.sub_image = track_sub_image(data, equirect.subImage);
    tracked.scale_x = equirect.scale.x;
    tracked.scale_y = equirect.scale.y;
    tracked.bias_x = equirect.bias.x;
    tracked.bias_y = equirect.bias.y;
    fill_depth_from_next_chain(data, equirect.next, tracked.sub_image);
    break;
  }
  case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR: {
    auto &equirect =
        reinterpret_cast<const XrCompositionLayerEquirect2KHR &>(layer);
    tracked.eye_visibility = equirect.eyeVisibility;
    tracked.pose = equirect.pose;
    tracked.sub_image = track_sub_image(data, equirect.subImage);
    tracked.radius = equirect.radius;
    tracked.central_angle = equirect.centralHorizontalAngle;
    tracked.upper_vertical_angle = equirect.upperVerticalAngle;
    tracked.lower_vertical_angle = equirect.lowerVerticalAngle;
    fill_depth_from_next_chain(data, equirect.next, tracked.sub_image);
    break;
  }
  case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR: {
    auto &cube = reinterpret_cast<const XrCompositionLayerCubeKHR &>(layer);
    tracked.eye_visibility = cube.eyeVisibility;
    tracked.pose.orientation = cube.orientation;
    tracked.sub_image.swapchain = cube.swapchain;
    tracked.sub_image.image_array_index = cube.imageArrayIndex;
    auto it = data->swapchains.find(cube.swapchain);
    if (it != data->swapchains.end()) {
      tracked.sub_image.extent_width = (int32_t)it->second.width;
      tracked.sub_image.extent_height = (int32_t)it->second.height;
      maybe_attach_known_image_index(it->second, tracked.sub_image);
    }
    break;
  }
  default:
    break;
  }

  return tracked;
}

static TrackedCompositionFrame
track_frame_layers(const InstanceData *data,
                   const XrFrameEndInfo *frame_end_info) {
  TrackedCompositionFrame frame;
  if (frame_end_info == nullptr)
    return frame;

  frame.display_time = frame_end_info->displayTime;
  frame.environment_blend_mode = frame_end_info->environmentBlendMode;
  frame.layers.reserve(frame_end_info->layerCount);
  for (uint32_t layer_index = 0; layer_index < frame_end_info->layerCount;
       ++layer_index) {
    auto *layer = frame_end_info->layers[layer_index];
    if (layer != nullptr)
      frame.layers.push_back(track_layer(data, *layer));
  }

  return frame;
}

// ── xrWaitFrame ──────────────────────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrWaitFrame(XrSession session,
                                      const XrFrameWaitInfo *frameWaitInfo,
                                      XrFrameState *frameState) {
  InstanceData *data = GetInstanceDataFromSession(session);
  if (data == nullptr)
    return XR_ERROR_HANDLE_INVALID;

  int64_t t0 = now_ns();
  XrResult result = data->next.xrWaitFrame(session, frameWaitInfo, frameState);
  int64_t t1 = now_ns();

  if (XR_SUCCEEDED(result) && frameState != nullptr) {
    std::unique_lock lock(data->state_mutex);

    // Original frame_state tracking (for Frame Info panel)
    data->frame_state.predicted_display_time = frameState->predictedDisplayTime;
    data->frame_state.predicted_display_period =
        frameState->predictedDisplayPeriod;
    data->frame_state.should_render = frameState->shouldRender;
    data->frame_state.frame_count++;

    XrTime new_time = frameState->predictedDisplayTime;
    if (data->frame_state.prev_display_time != 0 && new_time > 0) {
      float delta_ms =
          (float)((new_time - data->frame_state.prev_display_time) * 1e-6);
      auto &fs = data->frame_state;
      fs.timing_deltas_ms[fs.timing_write_idx] = delta_ms;
      fs.timing_write_idx =
          (fs.timing_write_idx + 1) % TrackedFrameState::kMaxTimingSamples;
      if (fs.timing_count < TrackedFrameState::kMaxTimingSamples)
        fs.timing_count++;
    }
    data->frame_state.prev_display_time = new_time;

    // Perf: xrWaitFrame marks the start of a new frame.
    // If there's an old incomplete current frame, discard it.
    auto &perf = data->perf;
    perf.current = {};
    perf.has_current = true;
    perf.current.frame_number = data->frame_state.frame_count;
    perf.current.wait_frame_call_ts = t0;
    perf.current.wait_frame_return_ts = t1;
    perf.current.predicted_display_time = frameState->predictedDisplayTime;
    perf.current.predicted_display_period = frameState->predictedDisplayPeriod;
  }

  return result;
}

// ── xrBeginFrame ─────────────────────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrBeginFrame(XrSession session,
                                       const XrFrameBeginInfo *frameBeginInfo) {
  InstanceData *data = GetInstanceDataFromSession(session);
  if (data == nullptr)
    return XR_ERROR_HANDLE_INVALID;

  int64_t t0 = now_ns();
  XrResult result = data->next.xrBeginFrame(session, frameBeginInfo);
  int64_t t1 = now_ns();

  if (XR_SUCCEEDED(result)) {
    std::unique_lock lock(data->state_mutex);
    if (data->perf.has_current) {
      data->perf.current.begin_frame_call_ts = t0;
      data->perf.current.begin_frame_return_ts = t1;
    }
  }

  return result;
}

// ── xrEndFrame ───────────────────────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrEndFrame(XrSession session,
                                     const XrFrameEndInfo *frameEndInfo) {
  InstanceData *data = GetInstanceDataFromSession(session);
  if (data == nullptr)
    return XR_ERROR_HANDLE_INVALID;

  TrackedCompositionFrame submitted_frame;

  int64_t t0 = now_ns();
  XrResult result = data->next.xrEndFrame(session, frameEndInfo);
  int64_t t1 = now_ns();

  {
    std::unique_lock lock(data->state_mutex);
    if (XR_SUCCEEDED(result) && frameEndInfo != nullptr) {
      submitted_frame = track_frame_layers(data, frameEndInfo);
      submitted_frame.frame_number = data->frame_state.frame_count;
      data->composition_frames.push_back(std::move(submitted_frame));
      while (data->composition_frames.size() > 32)
        data->composition_frames.pop_front();
    }

    auto &perf = data->perf;
    if (perf.has_current) {
      auto &c = perf.current;
      c.end_frame_call_ts = t0;
      c.end_frame_return_ts = t1;

      // Compute derived durations
      c.wait_frame_ms = ns_to_ms(c.wait_frame_call_ts, c.wait_frame_return_ts);
      c.begin_frame_ms =
          ns_to_ms(c.begin_frame_call_ts, c.begin_frame_return_ts);
      c.end_frame_ms = ns_to_ms(c.end_frame_call_ts, c.end_frame_return_ts);
      if (c.begin_frame_return_ts > 0 && c.end_frame_call_ts > 0)
        c.app_work_ms = ns_to_ms(c.begin_frame_return_ts, c.end_frame_call_ts);
      c.total_frame_ms = ns_to_ms(c.wait_frame_call_ts, c.end_frame_return_ts);
      if (c.sync_actions_call_ts > 0 && c.sync_actions_return_ts > 0)
        c.sync_actions_ms =
            ns_to_ms(c.sync_actions_call_ts, c.sync_actions_return_ts);
      if (c.locate_views_call_ts > 0 && c.locate_views_return_ts > 0)
        c.locate_views_ms =
            ns_to_ms(c.locate_views_call_ts, c.locate_views_return_ts);

      // Sum swapchain acquire+wait times
      float sc_total = 0.0f;
      for (auto &sc : c.swapchains) {
        if (sc.acquire_call_ts > 0 && sc.acquire_done_ts > 0)
          sc_total += ns_to_ms(sc.acquire_call_ts, sc.acquire_done_ts);
        if (sc.wait_call_ts > 0 && sc.wait_done_ts > 0)
          sc_total += ns_to_ms(sc.wait_call_ts, sc.wait_done_ts);
      }
      c.swapchain_acquire_wait_ms = sc_total;

      perf.push_completed_frame();
    }
  }

  return result;
}

// ── xrCreateSwapchain / xrDestroySwapchain ───────────────────────────────────

XrResult XRAPI_CALL Layer_xrCreateSwapchain(
    XrSession session, const XrSwapchainCreateInfo *createInfo,
    XrSwapchain *swapchain) {
  InstanceData *data = GetInstanceDataFromSession(session);
  if (data == nullptr)
    return XR_ERROR_HANDLE_INVALID;

  XrResult result =
      data->next.xrCreateSwapchain(session, createInfo, swapchain);
  if (XR_SUCCEEDED(result)) {
    RegisterSwapchain(*swapchain, data);
    std::unique_lock lock(data->state_mutex);
    TrackedSwapchain tracked;
    tracked.handle = *swapchain;
    tracked.session = session;
    tracked.format = createInfo != nullptr ? createInfo->format : 0;
    tracked.width = createInfo != nullptr ? createInfo->width : 0;
    tracked.height = createInfo != nullptr ? createInfo->height : 0;
    tracked.array_size = createInfo != nullptr ? createInfo->arraySize : 0;
    tracked.face_count = createInfo != nullptr ? createInfo->faceCount : 0;
    tracked.mip_count = createInfo != nullptr ? createInfo->mipCount : 0;
    tracked.sample_count = createInfo != nullptr ? createInfo->sampleCount : 0;
    tracked.usage_flags = createInfo != nullptr ? createInfo->usageFlags : 0;
    tracked.create_flags = createInfo != nullptr ? createInfo->createFlags : 0;
    data->swapchains[*swapchain] = std::move(tracked);
  }
  return result;
}

XrResult XRAPI_CALL Layer_xrEnumerateSwapchainImages(
    XrSwapchain swapchain, uint32_t imageCapacityInput,
    uint32_t *imageCountOutput, XrSwapchainImageBaseHeader *images) {
  InstanceData *data = GetInstanceDataFromSwapchain(swapchain);
  if (data == nullptr)
    return XR_ERROR_HANDLE_INVALID;

  XrResult result = data->next.xrEnumerateSwapchainImages(
      swapchain, imageCapacityInput, imageCountOutput, images);
  if (XR_SUCCEEDED(result) && images != nullptr &&
      imageCountOutput != nullptr) {
    std::unique_lock lock(data->state_mutex);
    auto it = data->swapchains.find(swapchain);
    if (it != data->swapchains.end()) {
      it->second.images.clear();
      it->second.images.reserve(*imageCountOutput);
      for (uint32_t image_index = 0; image_index < *imageCountOutput;
           ++image_index)
        it->second.images.push_back(track_swapchain_image(images[image_index]));
    }
  }

  return result;
}

XrResult XRAPI_CALL Layer_xrDestroySwapchain(XrSwapchain swapchain) {
  InstanceData *data = GetInstanceDataFromSwapchain(swapchain);
  if (data == nullptr)
    return XR_ERROR_HANDLE_INVALID;

  XrResult result = data->next.xrDestroySwapchain(swapchain);
  {
    std::unique_lock lock(data->state_mutex);
    data->swapchains.erase(swapchain);
  }
  UnregisterSwapchain(swapchain);
  return result;
}

// ── Swapchain image ops ──────────────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrAcquireSwapchainImage(
    XrSwapchain swapchain, const XrSwapchainImageAcquireInfo *acquireInfo,
    uint32_t *index) {
  InstanceData *data = GetInstanceDataFromSwapchain(swapchain);
  if (data == nullptr)
    return XR_ERROR_HANDLE_INVALID;

  int64_t t0 = now_ns();
  XrResult result =
      data->next.xrAcquireSwapchainImage(swapchain, acquireInfo, index);
  int64_t t1 = now_ns();

  if (XR_SUCCEEDED(result)) {
    std::unique_lock lock(data->state_mutex);
    auto tracked_it = data->swapchains.find(swapchain);
    if (tracked_it != data->swapchains.end() && index != nullptr) {
      tracked_it->second.has_latest_acquired_index = true;
      tracked_it->second.latest_acquired_index = *index;
    }
    if (data->perf.has_current) {
      SwapchainTiming sc;
      sc.acquire_call_ts = t0;
      sc.acquire_done_ts = t1;
      data->perf.current.swapchains.push_back(sc);
    }
  }
  return result;
}

XrResult XRAPI_CALL Layer_xrWaitSwapchainImage(
    XrSwapchain swapchain, const XrSwapchainImageWaitInfo *waitInfo) {
  InstanceData *data = GetInstanceDataFromSwapchain(swapchain);
  if (data == nullptr)
    return XR_ERROR_HANDLE_INVALID;

  int64_t t0 = now_ns();
  XrResult result = data->next.xrWaitSwapchainImage(swapchain, waitInfo);
  int64_t t1 = now_ns();

  if (XR_SUCCEEDED(result)) {
    std::unique_lock lock(data->state_mutex);
    if (data->perf.has_current && !data->perf.current.swapchains.empty()) {
      // Fill wait timing into the most recently acquired swapchain
      auto &sc = data->perf.current.swapchains.back();
      sc.wait_call_ts = t0;
      sc.wait_done_ts = t1;
    }
  }
  return result;
}

XrResult XRAPI_CALL Layer_xrReleaseSwapchainImage(
    XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *releaseInfo) {
  InstanceData *data = GetInstanceDataFromSwapchain(swapchain);
  if (data == nullptr)
    return XR_ERROR_HANDLE_INVALID;

  TrackedPreviewImage captured_preview;
  std::string preview_status;
  bool has_captured_preview = false;
  bool attempted_preview_capture = false;
  {
    std::shared_lock lock(data->state_mutex);
    auto tracked_it = data->swapchains.find(swapchain);
    if (tracked_it != data->swapchains.end()) {
      auto session_it = data->sessions.find(tracked_it->second.session);
      const char *skip_reason = preview_capture_skip_reason(
          session_it != data->sessions.end() ? &session_it->second : nullptr,
          tracked_it->second);
      if (skip_reason == nullptr) {
        attempted_preview_capture = true;
        has_captured_preview =
            capture_opengl_preview(tracked_it->second, captured_preview,
                                   preview_status);
      } else {
        preview_status = skip_reason;
      }
    }
  }

  int64_t t0 = now_ns();
  XrResult result = data->next.xrReleaseSwapchainImage(swapchain, releaseInfo);
  int64_t t1 = now_ns();

  if (XR_SUCCEEDED(result)) {
    std::string preview_log_line;
    std::unique_lock lock(data->state_mutex);
    auto tracked_it = data->swapchains.find(swapchain);
    if (tracked_it != data->swapchains.end() &&
        tracked_it->second.has_latest_acquired_index) {
      TrackedSwapchain &tracked = tracked_it->second;
      std::string previous_status = tracked.preview_status;
      bool had_preview = tracked.latest_preview.available;

      tracked_it->second.has_latest_released_index = true;
      tracked_it->second.latest_released_index =
          tracked_it->second.latest_acquired_index;
      tracked_it->second.release_serial++;
      if (has_captured_preview) {
        tracked.latest_preview = std::move(captured_preview);
        tracked.preview_images[make_preview_key(tracked.latest_preview.image_index,
                                               tracked.latest_preview.image_array_index)] =
            tracked.latest_preview;
      }

      if (attempted_preview_capture) {
        tracked.preview_attempt_count++;
        if (has_captured_preview)
          tracked.preview_success_count++;
      } else {
        tracked.preview_skip_count++;
      }

      if (has_captured_preview || attempted_preview_capture || !had_preview) {
        tracked.preview_status = preview_status;
      }

      int preview_log_level = get_preview_log_level();
      bool should_log = false;
      if (preview_log_level > 0) {
        if (preview_log_level > 1) {
          should_log = true;
        } else if (has_captured_preview && !tracked.preview_logged_success) {
          should_log = true;
          tracked.preview_logged_success = true;
        } else if (!has_captured_preview && !tracked.preview_status.empty() &&
                   std::find(tracked.preview_logged_failure_statuses.begin(),
                             tracked.preview_logged_failure_statuses.end(),
                             tracked.preview_status) ==
                       tracked.preview_logged_failure_statuses.end()) {
          should_log = true;
          tracked.preview_logged_failure_statuses.push_back(
              tracked.preview_status);
        }
      }

      if (should_log) {
        std::ostringstream oss;
        oss << "[XR_APILAYER_DEBUG_gui][preview] swapchain=0x" << std::hex
            << (uint64_t)swapchain << std::dec << " session=0x" << std::hex
            << (uint64_t)tracked.session << std::dec
            << " attempts=" << tracked.preview_attempt_count
            << " success=" << tracked.preview_success_count
            << " skipped=" << tracked.preview_skip_count
            << " status=\"" << tracked.preview_status << "\"";
        preview_log_line = oss.str();
      }
    }
    if (data->perf.has_current && !data->perf.current.swapchains.empty()) {
      auto &sc = data->perf.current.swapchains.back();
      sc.release_ts = t0;
      sc.release_done_ts = t1;
    }

    lock.unlock();
    if (!preview_log_line.empty())
      std::cout << preview_log_line << std::endl;
  }
  return result;
}

} // namespace debug_layer
