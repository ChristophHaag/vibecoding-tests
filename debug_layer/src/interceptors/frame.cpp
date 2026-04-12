// SPDX-License-Identifier: MIT
// interceptors/frame.cpp — xrWaitFrame, xrBeginFrame, xrEndFrame, swapchain
// ops.

#include "../dispatch.h"
#include "../instance_data.h"
#include "vulkan_preview_spv.h"

#include <EGL/egl.h>
#include <GL/glx.h>
#include <SDL3/SDL_opengl.h>

// clang-format off
#define XR_USE_GRAPHICS_API_OPENGL
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr_platform.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <ctime>
#if defined(__linux__)
#include <dlfcn.h>
#endif
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
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
  int opengl_inflight_slots = 4;
  int vulkan_inflight_slots = 4;
};

static int parse_env_int(const char *name, int fallback, int min_value,
                         int max_value) {
  const char *env = std::getenv(name);
  if (env == nullptr || env[0] == '\0')
    return fallback;

  int parsed = std::atoi(env);
  return std::clamp(parsed, min_value, max_value);
}

static int parse_env_int_with_legacy(const char *primary_name,
                                     const char *legacy_name, int fallback,
                                     int min_value, int max_value) {
  const char *env = std::getenv(primary_name);
  if ((env == nullptr || env[0] == '\0') && legacy_name != nullptr)
    env = std::getenv(legacy_name);

  if (env == nullptr || env[0] == '\0')
    return fallback;

  int parsed = std::atoi(env);
  return std::clamp(parsed, min_value, max_value);
}

static PreviewCaptureConfig get_preview_capture_config() {
  static PreviewCaptureConfig config = [] {
    PreviewCaptureConfig c;
    c.capture_interval = parse_env_int_with_legacy(
        "XR_DEBUG_GUI_PREVIEW_INTERVAL", "XR_DEBUG_GUI_GL_PREVIEW_INTERVAL", 2,
        0, 120);
    c.max_edge = parse_env_int_with_legacy("XR_DEBUG_GUI_PREVIEW_MAX_EDGE",
                                           "XR_DEBUG_GUI_GL_PREVIEW_MAX_EDGE",
                                           320, 32, 2048);
    c.opengl_inflight_slots = parse_env_int_with_legacy(
      "XR_DEBUG_GUI_PREVIEW_OPENGL_INFLIGHT",
      "XR_DEBUG_GUI_GL_PREVIEW_INFLIGHT", 4, 1, 16);
    c.vulkan_inflight_slots =
        parse_env_int("XR_DEBUG_GUI_PREVIEW_VULKAN_INFLIGHT", 4, 1, 16);
    return c;
  }();
  return config;
}

static void record_preview_timing(float sample_ms, float &last_ms,
                                  float &max_ms, double &total_ms,
                                  uint64_t &sample_count) {
  last_ms = sample_ms;
  max_ms = std::max(max_ms, sample_ms);
  total_ms += (double)sample_ms;
  sample_count++;
}

template <typename Handle> static uint64_t pack_handle(Handle handle) {
  if constexpr (std::is_pointer_v<Handle>)
    return (uint64_t)reinterpret_cast<uintptr_t>(handle);
  else
    return (uint64_t)handle;
}

template <typename Handle> static Handle unpack_handle(uint64_t value) {
  if constexpr (std::is_pointer_v<Handle>)
    return reinterpret_cast<Handle>((uintptr_t)value);
  else
    return (Handle)value;
}

struct PreviewCaptureRequest {
  XrSession session = XR_NULL_HANDLE;
  XrSwapchain swapchain = XR_NULL_HANDLE;
  TrackedSession::GraphicsBindingKind graphics_binding =
      TrackedSession::GraphicsBindingKind::UNKNOWN;
  XrSwapchainUsageFlags usage_flags = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t array_size = 0;
  uint32_t face_count = 0;
  uint32_t sample_count = 0;
  int64_t format = 0;
  uint64_t release_serial = 0;
  bool has_latest_acquired_index = false;
  uint32_t image_index = 0;
  XrStructureType image_type = XR_TYPE_UNKNOWN;
  uint64_t image_handle_value = 0;
};

static uint64_t make_preview_key(uint32_t image_index,
                                 uint32_t image_array_index) {
  return ((uint64_t)image_array_index << 32) | (uint64_t)image_index;
}

static int get_preview_log_level() {
  static int level = parse_env_int_with_legacy(
      "XR_DEBUG_GUI_PREVIEW_LOG", "XR_DEBUG_GUI_GL_PREVIEW_LOG", 0, 0, 2);
  return level;
}

struct VulkanPreviewSessionBinding {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  uint32_t queue_family_index = 0;
  uint32_t queue_index = 0;
};

struct VulkanPreviewCaptureSlot {
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  VkImage destination_image = VK_NULL_HANDLE;
  VkDeviceMemory destination_memory = VK_NULL_HANDLE;
  VkImageView destination_image_view = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkBuffer staging_buffer = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory = VK_NULL_HANDLE;
  void *mapped_staging = nullptr;
  bool staging_memory_coherent = false;
  VkDeviceSize staging_buffer_size = 0;
  uint32_t destination_width = 0;
  uint32_t destination_height = 0;
  VkImageView source_image_view = VK_NULL_HANDLE;
  bool in_flight = false;
  XrSwapchain swapchain = XR_NULL_HANDLE;
  uint32_t image_index = 0;
  uint32_t image_array_index = 0;
  uint64_t capture_serial = 0;
  uint32_t preview_width = 0;
  uint32_t preview_height = 0;
  int64_t submit_begin_ns = 0;
  int64_t submit_end_ns = 0;
};

struct VulkanPreviewSubmitResult {
  bool submitted = false;
  float app_thread_ms = 0.0f;
  std::string status;
};

struct VulkanPreviewHarvestResult {
  XrSwapchain swapchain = XR_NULL_HANDLE;
  TrackedPreviewImage preview;
  std::string status;
  float finalize_ms = 0.0f;
  float ready_latency_ms = 0.0f;
};

struct OpenGLPreviewCaptureSlot {
  GLuint src_fbo = 0;
  GLuint dst_fbo = 0;
  GLuint dst_texture = 0;
  GLuint pbo = 0;
  GLsync fence = nullptr;
  bool in_flight = false;
  uint32_t preview_width = 0;
  uint32_t preview_height = 0;
  size_t byte_size = 0;
  XrSwapchain swapchain = XR_NULL_HANDLE;
  uint32_t image_index = 0;
  uint32_t image_array_index = 0;
  uint64_t capture_serial = 0;
  int64_t submit_begin_ns = 0;
  int64_t submit_end_ns = 0;
};

struct OpenGLPreviewContextState {
  uint64_t context_key = 0;
  std::vector<OpenGLPreviewCaptureSlot> slots;
};

struct OpenGLPreviewSessionState {
  std::mutex mutex;
  std::unordered_map<uint64_t, std::unique_ptr<OpenGLPreviewContextState>>
      contexts;
};

struct OpenGLPreviewSubmitResult {
  bool submitted = false;
  float app_thread_ms = 0.0f;
  std::string status;
};

struct OpenGLPreviewHarvestResult {
  XrSwapchain swapchain = XR_NULL_HANDLE;
  TrackedPreviewImage preview;
  std::string status;
  float finalize_ms = 0.0f;
  float ready_latency_ms = 0.0f;
};

struct VulkanPreviewSessionState {
  std::mutex mutex;
  VulkanPreviewSessionBinding binding;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkShaderModule vertex_shader = VK_NULL_HANDLE;
  VkShaderModule fragment_shader = VK_NULL_HANDLE;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  bool static_resources_ready = false;
  std::vector<VulkanPreviewCaptureSlot> slots;
};

static std::mutex g_vulkan_preview_sessions_mutex;
static std::unordered_map<XrSession, std::shared_ptr<VulkanPreviewSessionState>>
    g_vulkan_preview_sessions;

static std::mutex g_opengl_preview_sessions_mutex;
static std::unordered_map<XrSession, std::shared_ptr<OpenGLPreviewSessionState>>
  g_opengl_preview_sessions;

static struct {
  bool loaded = false;
  PFNGLGENFRAMEBUFFERSPROC GenFramebuffers = nullptr;
  PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = nullptr;
  PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D = nullptr;
  PFNGLFRAMEBUFFERTEXTURELAYERPROC FramebufferTextureLayer = nullptr;
  PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers = nullptr;
  PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus = nullptr;
  PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer = nullptr;
  PFNGLGENBUFFERSPROC GenBuffers = nullptr;
  PFNGLBINDBUFFERPROC BindBuffer = nullptr;
  PFNGLBUFFERDATAPROC BufferData = nullptr;
  PFNGLDELETEBUFFERSPROC DeleteBuffers = nullptr;
  PFNGLMAPBUFFERRANGEPROC MapBufferRange = nullptr;
  PFNGLUNMAPBUFFERPROC UnmapBuffer = nullptr;
  PFNGLFENCESYNCPROC FenceSync = nullptr;
  PFNGLCLIENTWAITSYNCPROC ClientWaitSync = nullptr;
  PFNGLDELETESYNCPROC DeleteSync = nullptr;
} preview_gl;

#if defined(__linux__)
static void *load_egl_symbol(const char *name) {
  static void *egl_handle = []() -> void * {
    void *handle = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (handle == nullptr)
      handle = dlopen("libEGL.so", RTLD_LAZY | RTLD_LOCAL);
    return handle;
  }();
  return egl_handle != nullptr ? dlsym(egl_handle, name) : nullptr;
}
#endif

static uint64_t current_gl_context_key() {
#if defined(__linux__)
  using EglGetCurrentContextProc = EGLContext (*)(void);
  static EglGetCurrentContextProc egl_get_current_context =
      reinterpret_cast<EglGetCurrentContextProc>(
          load_egl_symbol("eglGetCurrentContext"));
  if (egl_get_current_context != nullptr) {
    EGLContext egl_context = egl_get_current_context();
    if (egl_context != EGL_NO_CONTEXT)
      return pack_handle(egl_context);
  }

  GLXContext glx_context = glXGetCurrentContext();
  if (glx_context != nullptr)
    return pack_handle(glx_context);
#endif
  return 0;
}

static void *get_preview_gl_proc_address(const char *name) {
  void *proc = nullptr;

#if defined(XR_USE_PLATFORM_EGL) || defined(__linux__)
  using EglGetProcAddressProc = __eglMustCastToProperFunctionPointerType (*)(const char *);
  static EglGetProcAddressProc egl_get_proc_address =
      reinterpret_cast<EglGetProcAddressProc>(
          load_egl_symbol("eglGetProcAddress"));
  if (egl_get_proc_address != nullptr) {
    proc = reinterpret_cast<void *>(egl_get_proc_address(name));
    if (proc != nullptr)
      return proc;
  }
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
          preview_gl.BlitFramebuffer != nullptr &&
          preview_gl.GenBuffers != nullptr &&
          preview_gl.BindBuffer != nullptr &&
          preview_gl.BufferData != nullptr &&
          preview_gl.DeleteBuffers != nullptr &&
          preview_gl.MapBufferRange != nullptr &&
          preview_gl.UnmapBuffer != nullptr &&
          preview_gl.FenceSync != nullptr &&
          preview_gl.ClientWaitSync != nullptr &&
          preview_gl.DeleteSync != nullptr;
  }

#define LOAD(name)                                                             \
  preview_gl.name =                                                            \
      (decltype(preview_gl.name))get_preview_gl_proc_address("gl" #name)
  LOAD(GenFramebuffers);
  LOAD(BindFramebuffer);
  LOAD(FramebufferTexture2D);
  LOAD(FramebufferTextureLayer);
  LOAD(DeleteFramebuffers);
  LOAD(CheckFramebufferStatus);
  LOAD(BlitFramebuffer);
  LOAD(GenBuffers);
  LOAD(BindBuffer);
  LOAD(BufferData);
  LOAD(DeleteBuffers);
  LOAD(MapBufferRange);
  LOAD(UnmapBuffer);
  LOAD(FenceSync);
  LOAD(ClientWaitSync);
  LOAD(DeleteSync);
#undef LOAD

  preview_gl.loaded = true;
  return preview_gl.GenFramebuffers != nullptr &&
         preview_gl.BindFramebuffer != nullptr &&
         preview_gl.FramebufferTexture2D != nullptr &&
         preview_gl.FramebufferTextureLayer != nullptr &&
         preview_gl.DeleteFramebuffers != nullptr &&
         preview_gl.CheckFramebufferStatus != nullptr &&
         preview_gl.BlitFramebuffer != nullptr &&
         preview_gl.GenBuffers != nullptr &&
         preview_gl.BindBuffer != nullptr &&
         preview_gl.BufferData != nullptr &&
         preview_gl.DeleteBuffers != nullptr &&
         preview_gl.MapBufferRange != nullptr &&
         preview_gl.UnmapBuffer != nullptr &&
         preview_gl.FenceSync != nullptr &&
         preview_gl.ClientWaitSync != nullptr &&
         preview_gl.DeleteSync != nullptr;
}

static std::shared_ptr<VulkanPreviewSessionState>
get_vulkan_preview_session_state(XrSession session) {
  std::lock_guard lock(g_vulkan_preview_sessions_mutex);
  auto it = g_vulkan_preview_sessions.find(session);
  return it != g_vulkan_preview_sessions.end() ? it->second : nullptr;
}

static std::shared_ptr<OpenGLPreviewSessionState>
get_opengl_preview_session_state(XrSession session, bool create_if_missing) {
  std::lock_guard lock(g_opengl_preview_sessions_mutex);
  auto it = g_opengl_preview_sessions.find(session);
  if (it != g_opengl_preview_sessions.end())
    return it->second;
  if (!create_if_missing)
    return nullptr;

  auto state = std::make_shared<OpenGLPreviewSessionState>();
  g_opengl_preview_sessions[session] = state;
  return state;
}

static void destroy_opengl_preview_slot_resources(
    OpenGLPreviewCaptureSlot &slot) {
  if (slot.fence != nullptr)
    preview_gl.DeleteSync(slot.fence);
  if (slot.pbo != 0)
    preview_gl.DeleteBuffers(1, &slot.pbo);
  if (slot.dst_fbo != 0)
    preview_gl.DeleteFramebuffers(1, &slot.dst_fbo);
  if (slot.src_fbo != 0)
    preview_gl.DeleteFramebuffers(1, &slot.src_fbo);
  if (slot.dst_texture != 0)
    glDeleteTextures(1, &slot.dst_texture);

  slot = OpenGLPreviewCaptureSlot{};
}

static void destroy_opengl_preview_context_resources(
    OpenGLPreviewContextState &context_state) {
  for (auto &slot : context_state.slots)
    destroy_opengl_preview_slot_resources(slot);
  context_state.slots.clear();
}

static OpenGLPreviewContextState *
get_or_create_opengl_preview_context_state(OpenGLPreviewSessionState &state,
                                           uint64_t context_key) {
  if (context_key == 0)
    return nullptr;

  auto &entry = state.contexts[context_key];
  if (!entry) {
    entry = std::make_unique<OpenGLPreviewContextState>();
    entry->context_key = context_key;
    entry->slots.resize((size_t)get_preview_capture_config().opengl_inflight_slots);
  }
  return entry.get();
}

static void destroy_vulkan_preview_slot_submission_resources(
    VkDevice device, VulkanPreviewCaptureSlot &slot) {
  if (device == VK_NULL_HANDLE)
    return;

  if (slot.source_image_view != VK_NULL_HANDLE)
    vkDestroyImageView(device, slot.source_image_view, nullptr);

  slot.source_image_view = VK_NULL_HANDLE;
  slot.in_flight = false;
  slot.swapchain = XR_NULL_HANDLE;
  slot.image_index = 0;
  slot.image_array_index = 0;
  slot.capture_serial = 0;
  slot.preview_width = 0;
  slot.preview_height = 0;
  slot.submit_begin_ns = 0;
  slot.submit_end_ns = 0;
}

static void destroy_vulkan_preview_slot_target_resources(
    VkDevice device, VulkanPreviewCaptureSlot &slot) {
  if (device == VK_NULL_HANDLE)
    return;

  if (slot.mapped_staging != nullptr && slot.staging_memory != VK_NULL_HANDLE) {
    vkUnmapMemory(device, slot.staging_memory);
    slot.mapped_staging = nullptr;
  }
  if (slot.framebuffer != VK_NULL_HANDLE)
    vkDestroyFramebuffer(device, slot.framebuffer, nullptr);
  if (slot.destination_image_view != VK_NULL_HANDLE)
    vkDestroyImageView(device, slot.destination_image_view, nullptr);
  if (slot.destination_image != VK_NULL_HANDLE)
    vkDestroyImage(device, slot.destination_image, nullptr);
  if (slot.destination_memory != VK_NULL_HANDLE)
    vkFreeMemory(device, slot.destination_memory, nullptr);
  if (slot.staging_buffer != VK_NULL_HANDLE)
    vkDestroyBuffer(device, slot.staging_buffer, nullptr);
  if (slot.staging_memory != VK_NULL_HANDLE)
    vkFreeMemory(device, slot.staging_memory, nullptr);

  slot.framebuffer = VK_NULL_HANDLE;
  slot.destination_image_view = VK_NULL_HANDLE;
  slot.destination_image = VK_NULL_HANDLE;
  slot.destination_memory = VK_NULL_HANDLE;
  slot.staging_buffer = VK_NULL_HANDLE;
  slot.staging_memory = VK_NULL_HANDLE;
  slot.staging_buffer_size = 0;
  slot.destination_width = 0;
  slot.destination_height = 0;
  slot.staging_memory_coherent = false;
}

static void
destroy_vulkan_preview_static_resources(VulkanPreviewSessionState &state) {
  VkDevice device = state.binding.device;
  if (device == VK_NULL_HANDLE)
    return;

  for (auto &slot : state.slots) {
    if (slot.in_flight && slot.fence != VK_NULL_HANDLE)
      vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
    destroy_vulkan_preview_slot_submission_resources(device, slot);
    destroy_vulkan_preview_slot_target_resources(device, slot);
    if (slot.fence != VK_NULL_HANDLE)
      vkDestroyFence(device, slot.fence, nullptr);
    slot.fence = VK_NULL_HANDLE;
    slot.command_buffer = VK_NULL_HANDLE;
    slot.descriptor_set = VK_NULL_HANDLE;
  }
  state.slots.clear();

  if (state.pipeline != VK_NULL_HANDLE)
    vkDestroyPipeline(device, state.pipeline, nullptr);
  if (state.render_pass != VK_NULL_HANDLE)
    vkDestroyRenderPass(device, state.render_pass, nullptr);
  if (state.fragment_shader != VK_NULL_HANDLE)
    vkDestroyShaderModule(device, state.fragment_shader, nullptr);
  if (state.vertex_shader != VK_NULL_HANDLE)
    vkDestroyShaderModule(device, state.vertex_shader, nullptr);
  if (state.pipeline_layout != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(device, state.pipeline_layout, nullptr);
  if (state.descriptor_pool != VK_NULL_HANDLE)
    vkDestroyDescriptorPool(device, state.descriptor_pool, nullptr);
  if (state.descriptor_set_layout != VK_NULL_HANDLE)
    vkDestroyDescriptorSetLayout(device, state.descriptor_set_layout, nullptr);
  if (state.sampler != VK_NULL_HANDLE)
    vkDestroySampler(device, state.sampler, nullptr);
  if (state.command_pool != VK_NULL_HANDLE)
    vkDestroyCommandPool(device, state.command_pool, nullptr);

  state.pipeline = VK_NULL_HANDLE;
  state.render_pass = VK_NULL_HANDLE;
  state.fragment_shader = VK_NULL_HANDLE;
  state.vertex_shader = VK_NULL_HANDLE;
  state.pipeline_layout = VK_NULL_HANDLE;
  state.descriptor_pool = VK_NULL_HANDLE;
  state.descriptor_set_layout = VK_NULL_HANDLE;
  state.sampler = VK_NULL_HANDLE;
  state.command_pool = VK_NULL_HANDLE;
  state.queue = VK_NULL_HANDLE;
  state.static_resources_ready = false;
}

static bool find_vulkan_memory_type(VkPhysicalDevice physical_device,
                                    uint32_t type_bits,
                                    VkMemoryPropertyFlags required_flags,
                                    VkMemoryPropertyFlags preferred_flags,
                                    uint32_t &type_index_out,
                                    bool &preferred_flags_available) {
  if (physical_device == VK_NULL_HANDLE)
    return false;

  VkPhysicalDeviceMemoryProperties memory_properties;
  vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

  int fallback_index = -1;
  for (uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
    if ((type_bits & (1u << index)) == 0)
      continue;

    VkMemoryPropertyFlags property_flags =
        memory_properties.memoryTypes[index].propertyFlags;
    if ((property_flags & required_flags) != required_flags)
      continue;

    if ((property_flags & preferred_flags) == preferred_flags) {
      type_index_out = index;
      preferred_flags_available = true;
      return true;
    }

    if (fallback_index < 0)
      fallback_index = (int)index;
  }

  if (fallback_index < 0)
    return false;

  type_index_out = (uint32_t)fallback_index;
  preferred_flags_available = preferred_flags == 0;
  return true;
}

static bool create_vulkan_shader_module(VkDevice device,
                                        const unsigned char *shader_bytes,
                                        size_t shader_size,
                                        VkShaderModule &shader_module) {
  VkShaderModuleCreateInfo create_info{
      VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  create_info.codeSize = shader_size;
  create_info.pCode = reinterpret_cast<const uint32_t *>(shader_bytes);
  return vkCreateShaderModule(device, &create_info, nullptr, &shader_module) ==
         VK_SUCCESS;
}

static bool
ensure_vulkan_preview_static_resources(VulkanPreviewSessionState &state,
                                       std::string &status) {
  if (state.static_resources_ready)
    return true;

  if (state.binding.device == VK_NULL_HANDLE ||
      state.binding.physical_device == VK_NULL_HANDLE) {
    status = "Vulkan preview session binding is incomplete";
    return false;
  }

  VkDevice device = state.binding.device;
  vkGetDeviceQueue(device, state.binding.queue_family_index,
                   state.binding.queue_index, &state.queue);
  if (state.queue == VK_NULL_HANDLE) {
    status = "Failed to retrieve the Vulkan queue from xrCreateSession";
    return false;
  }

  VkCommandPoolCreateInfo command_pool_info{
      VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  command_pool_info.queueFamilyIndex = state.binding.queue_family_index;
  if (vkCreateCommandPool(device, &command_pool_info, nullptr,
                          &state.command_pool) != VK_SUCCESS) {
    status = "Failed to create the Vulkan preview command pool";
    destroy_vulkan_preview_static_resources(state);
    return false;
  }

  VkCommandBufferAllocateInfo command_buffer_info{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  command_buffer_info.commandPool = state.command_pool;
  command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  PreviewCaptureConfig config = get_preview_capture_config();
  const uint32_t slot_count = (uint32_t)config.vulkan_inflight_slots;
  state.slots.assign(slot_count, VulkanPreviewCaptureSlot{});

  std::vector<VkCommandBuffer> command_buffers(slot_count, VK_NULL_HANDLE);
  command_buffer_info.commandBufferCount = slot_count;
  if (vkAllocateCommandBuffers(device, &command_buffer_info,
                               command_buffers.data()) != VK_SUCCESS) {
    status = "Failed to allocate the Vulkan preview command buffers";
    destroy_vulkan_preview_static_resources(state);
    return false;
  }

  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  for (uint32_t slot_index = 0; slot_index < slot_count; ++slot_index) {
    state.slots[slot_index].command_buffer = command_buffers[slot_index];
    if (vkCreateFence(device, &fence_info, nullptr,
                      &state.slots[slot_index].fence) != VK_SUCCESS) {
      status = "Failed to create a Vulkan preview fence";
      destroy_vulkan_preview_static_resources(state);
      return false;
    }
  }

  VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_NEAREST;
  sampler_info.minFilter = VK_FILTER_NEAREST;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.minLod = 0.0f;
  sampler_info.maxLod = 0.0f;
  if (vkCreateSampler(device, &sampler_info, nullptr, &state.sampler) !=
      VK_SUCCESS) {
    status = "Failed to create the Vulkan preview sampler";
    destroy_vulkan_preview_static_resources(state);
    return false;
  }

  VkDescriptorSetLayoutBinding descriptor_binding{};
  descriptor_binding.binding = 0;
  descriptor_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  descriptor_binding.descriptorCount = 1;
  descriptor_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  descriptor_set_layout_info.bindingCount = 1;
  descriptor_set_layout_info.pBindings = &descriptor_binding;
  if (vkCreateDescriptorSetLayout(device, &descriptor_set_layout_info, nullptr,
                                  &state.descriptor_set_layout) != VK_SUCCESS) {
    status = "Failed to create the Vulkan preview descriptor set layout";
    destroy_vulkan_preview_static_resources(state);
    return false;
  }

  VkDescriptorPoolSize descriptor_pool_size{};
  descriptor_pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  descriptor_pool_size.descriptorCount = slot_count;

  VkDescriptorPoolCreateInfo descriptor_pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  descriptor_pool_info.maxSets = slot_count;
  descriptor_pool_info.poolSizeCount = 1;
  descriptor_pool_info.pPoolSizes = &descriptor_pool_size;
  if (vkCreateDescriptorPool(device, &descriptor_pool_info, nullptr,
                             &state.descriptor_pool) != VK_SUCCESS) {
    status = "Failed to create the Vulkan preview descriptor pool";
    destroy_vulkan_preview_static_resources(state);
    return false;
  }

  VkDescriptorSetAllocateInfo descriptor_set_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  descriptor_set_info.descriptorPool = state.descriptor_pool;
  descriptor_set_info.descriptorSetCount = slot_count;
  std::vector<VkDescriptorSetLayout> set_layouts(slot_count,
                                                 state.descriptor_set_layout);
  descriptor_set_info.pSetLayouts = set_layouts.data();
  std::vector<VkDescriptorSet> descriptor_sets(slot_count, VK_NULL_HANDLE);
  if (vkAllocateDescriptorSets(device, &descriptor_set_info,
                               descriptor_sets.data()) != VK_SUCCESS) {
    status = "Failed to allocate the Vulkan preview descriptor sets";
    destroy_vulkan_preview_static_resources(state);
    return false;
  }
  for (uint32_t slot_index = 0; slot_index < slot_count; ++slot_index)
    state.slots[slot_index].descriptor_set = descriptor_sets[slot_index];

  if (!create_vulkan_shader_module(device, kVulkanPreviewVertexShaderSpv,
                                   sizeof(kVulkanPreviewVertexShaderSpv),
                                   state.vertex_shader)) {
    status = "Failed to create the Vulkan preview vertex shader module";
    destroy_vulkan_preview_static_resources(state);
    return false;
  }
  if (!create_vulkan_shader_module(device, kVulkanPreviewFragmentShaderSpv,
                                   sizeof(kVulkanPreviewFragmentShaderSpv),
                                   state.fragment_shader)) {
    status = "Failed to create the Vulkan preview fragment shader module";
    destroy_vulkan_preview_static_resources(state);
    return false;
  }

  VkAttachmentDescription color_attachment{};
  color_attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
  color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color_attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color_attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference color_attachment_ref{};
  color_attachment_ref.attachment = 0;
  color_attachment_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_attachment_ref;

  VkRenderPassCreateInfo render_pass_info{
      VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  render_pass_info.attachmentCount = 1;
  render_pass_info.pAttachments = &color_attachment;
  render_pass_info.subpassCount = 1;
  render_pass_info.pSubpasses = &subpass;
  if (vkCreateRenderPass(device, &render_pass_info, nullptr,
                         &state.render_pass) != VK_SUCCESS) {
    status = "Failed to create the Vulkan preview render pass";
    destroy_vulkan_preview_static_resources(state);
    return false;
  }

  VkPipelineLayoutCreateInfo pipeline_layout_info{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &state.descriptor_set_layout;
  if (vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
                             &state.pipeline_layout) != VK_SUCCESS) {
    status = "Failed to create the Vulkan preview pipeline layout";
    destroy_vulkan_preview_static_resources(state);
    return false;
  }

  VkPipelineShaderStageCreateInfo shader_stages[2]{};
  shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  shader_stages[0].module = state.vertex_shader;
  shader_stages[0].pName = "main";
  shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  shader_stages[1].module = state.fragment_shader;
  shader_stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertex_input_info{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

  VkPipelineInputAssemblyStateCreateInfo input_assembly_info{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly_info.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewport_state_info{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport_state_info.viewportCount = 1;
  viewport_state_info.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterization_info{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rasterization_info.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization_info.cullMode = VK_CULL_MODE_NONE;
  rasterization_info.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterization_info.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample_info{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample_info.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState color_blend_attachment{};
  color_blend_attachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  VkPipelineColorBlendStateCreateInfo color_blend_info{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  color_blend_info.attachmentCount = 1;
  color_blend_info.pAttachments = &color_blend_attachment;

  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                     VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic_state_info{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic_state_info.dynamicStateCount = 2;
  dynamic_state_info.pDynamicStates = dynamic_states;

  VkGraphicsPipelineCreateInfo pipeline_info{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipeline_info.stageCount = 2;
  pipeline_info.pStages = shader_stages;
  pipeline_info.pVertexInputState = &vertex_input_info;
  pipeline_info.pInputAssemblyState = &input_assembly_info;
  pipeline_info.pViewportState = &viewport_state_info;
  pipeline_info.pRasterizationState = &rasterization_info;
  pipeline_info.pMultisampleState = &multisample_info;
  pipeline_info.pColorBlendState = &color_blend_info;
  pipeline_info.pDynamicState = &dynamic_state_info;
  pipeline_info.layout = state.pipeline_layout;
  pipeline_info.renderPass = state.render_pass;
  pipeline_info.subpass = 0;
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                nullptr, &state.pipeline) != VK_SUCCESS) {
    status = "Failed to create the Vulkan preview pipeline";
    destroy_vulkan_preview_static_resources(state);
    return false;
  }

  state.static_resources_ready = true;
  return true;
}

static bool ensure_vulkan_preview_target_resources(
    VulkanPreviewSessionState &state, VulkanPreviewCaptureSlot &slot,
    uint32_t preview_width,
    uint32_t preview_height, bool &recreated, std::string &status) {
  recreated = false;
  if (slot.destination_image != VK_NULL_HANDLE &&
      slot.destination_width == preview_width &&
      slot.destination_height == preview_height &&
      slot.staging_buffer != VK_NULL_HANDLE && slot.mapped_staging != nullptr)
    return true;

  destroy_vulkan_preview_slot_target_resources(state.binding.device, slot);
  recreated = true;

  VkDevice device = state.binding.device;
  VkDeviceSize staging_size =
      (VkDeviceSize)preview_width * (VkDeviceSize)preview_height * 4;

  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.extent = {preview_width, preview_height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device, &image_info, nullptr, &slot.destination_image) !=
      VK_SUCCESS) {
    status = "Failed to create the Vulkan preview destination image";
    destroy_vulkan_preview_slot_target_resources(device, slot);
    return false;
  }

  VkMemoryRequirements image_memory_requirements;
  vkGetImageMemoryRequirements(device, slot.destination_image,
                               &image_memory_requirements);
  uint32_t image_memory_type_index = 0;
  bool image_memory_preferred = false;
  if (!find_vulkan_memory_type(state.binding.physical_device,
                               image_memory_requirements.memoryTypeBits,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                               image_memory_type_index,
                               image_memory_preferred)) {
    status = "Failed to find device-local Vulkan memory for the preview image";
    destroy_vulkan_preview_slot_target_resources(device, slot);
    return false;
  }

  VkMemoryAllocateInfo image_memory_info{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  image_memory_info.allocationSize = image_memory_requirements.size;
  image_memory_info.memoryTypeIndex = image_memory_type_index;
  if (vkAllocateMemory(device, &image_memory_info, nullptr,
                       &slot.destination_memory) != VK_SUCCESS) {
    status = "Failed to allocate Vulkan memory for the preview image";
    destroy_vulkan_preview_slot_target_resources(device, slot);
    return false;
  }
  if (vkBindImageMemory(device, slot.destination_image, slot.destination_memory,
                        0) != VK_SUCCESS) {
    status = "Failed to bind Vulkan memory for the preview image";
    destroy_vulkan_preview_slot_target_resources(device, slot);
    return false;
  }

  VkImageViewCreateInfo image_view_info{
      VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  image_view_info.image = slot.destination_image;
  image_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  image_view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  image_view_info.subresourceRange.baseMipLevel = 0;
  image_view_info.subresourceRange.levelCount = 1;
  image_view_info.subresourceRange.baseArrayLayer = 0;
  image_view_info.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device, &image_view_info, nullptr,
                        &slot.destination_image_view) != VK_SUCCESS) {
    status = "Failed to create the Vulkan preview destination image view";
    destroy_vulkan_preview_slot_target_resources(device, slot);
    return false;
  }

  VkFramebufferCreateInfo framebuffer_info{
      VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  framebuffer_info.renderPass = state.render_pass;
  framebuffer_info.attachmentCount = 1;
  framebuffer_info.pAttachments = &slot.destination_image_view;
  framebuffer_info.width = preview_width;
  framebuffer_info.height = preview_height;
  framebuffer_info.layers = 1;
  if (vkCreateFramebuffer(device, &framebuffer_info, nullptr,
                          &slot.framebuffer) != VK_SUCCESS) {
    status = "Failed to create the Vulkan preview framebuffer";
    destroy_vulkan_preview_slot_target_resources(device, slot);
    return false;
  }

  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = staging_size;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (vkCreateBuffer(device, &buffer_info, nullptr, &slot.staging_buffer) !=
      VK_SUCCESS) {
    status = "Failed to create the Vulkan preview staging buffer";
    destroy_vulkan_preview_slot_target_resources(device, slot);
    return false;
  }

  VkMemoryRequirements buffer_memory_requirements;
  vkGetBufferMemoryRequirements(device, slot.staging_buffer,
                                &buffer_memory_requirements);
  uint32_t buffer_memory_type_index = 0;
  bool host_coherent = false;
  if (!find_vulkan_memory_type(state.binding.physical_device,
                               buffer_memory_requirements.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               buffer_memory_type_index, host_coherent)) {
    status = "Failed to find host-visible Vulkan memory for the staging buffer";
    destroy_vulkan_preview_slot_target_resources(device, slot);
    return false;
  }

  VkMemoryAllocateInfo buffer_memory_info{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  buffer_memory_info.allocationSize = buffer_memory_requirements.size;
  buffer_memory_info.memoryTypeIndex = buffer_memory_type_index;
  if (vkAllocateMemory(device, &buffer_memory_info, nullptr,
                       &slot.staging_memory) != VK_SUCCESS) {
    status = "Failed to allocate Vulkan memory for the staging buffer";
    destroy_vulkan_preview_slot_target_resources(device, slot);
    return false;
  }
  if (vkBindBufferMemory(device, slot.staging_buffer, slot.staging_memory,
                         0) != VK_SUCCESS) {
    status = "Failed to bind Vulkan memory for the staging buffer";
    destroy_vulkan_preview_slot_target_resources(device, slot);
    return false;
  }
  if (vkMapMemory(device, slot.staging_memory, 0, staging_size, 0,
                  &slot.mapped_staging) != VK_SUCCESS) {
    status = "Failed to map the Vulkan preview staging memory";
    destroy_vulkan_preview_slot_target_resources(device, slot);
    return false;
  }

  slot.staging_memory_coherent = host_coherent;
  slot.staging_buffer_size = staging_size;
  slot.destination_width = preview_width;
  slot.destination_height = preview_height;
  return true;
}

static bool infer_vulkan_source_layout(const PreviewCaptureRequest &request,
                                       VkImageLayout &layout_out) {
  if ((request.usage_flags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0) {
    layout_out = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    return true;
  }
  if ((request.usage_flags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) != 0) {
    layout_out = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
  }
  return false;
}

void register_preview_capture_session_binding(
    XrSession session, const XrSessionCreateInfo *create_info) {
  if (session == XR_NULL_HANDLE || create_info == nullptr)
    return;

  const XrBaseInStructure *current =
      reinterpret_cast<const XrBaseInStructure *>(create_info->next);
  while (current != nullptr) {
    if (current->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR ||
        current->type == XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR) {
      auto *binding =
          reinterpret_cast<const XrGraphicsBindingVulkanKHR *>(current);
      std::shared_ptr<VulkanPreviewSessionState> state;
      {
        std::lock_guard lock(g_vulkan_preview_sessions_mutex);
        auto &slot = g_vulkan_preview_sessions[session];
        if (!slot)
          slot = std::make_shared<VulkanPreviewSessionState>();
        state = slot;
      }

      std::lock_guard capture_lock(state->mutex);
      destroy_vulkan_preview_static_resources(*state);
      state->binding.instance = binding->instance;
      state->binding.physical_device = binding->physicalDevice;
      state->binding.device = binding->device;
      state->binding.queue_family_index = binding->queueFamilyIndex;
      state->binding.queue_index = binding->queueIndex;
      return;
    }
    current = current->next;
  }
}

void destroy_preview_capture_session_resources(XrSession session) {
  std::shared_ptr<VulkanPreviewSessionState> state;
  {
    std::lock_guard lock(g_vulkan_preview_sessions_mutex);
    auto it = g_vulkan_preview_sessions.find(session);
    if (it != g_vulkan_preview_sessions.end()) {
      state = it->second;
      g_vulkan_preview_sessions.erase(it);
    }
  }

  if (state) {
    std::lock_guard capture_lock(state->mutex);
    destroy_vulkan_preview_static_resources(*state);
  }

  std::shared_ptr<OpenGLPreviewSessionState> opengl_state;
  {
    std::lock_guard lock(g_opengl_preview_sessions_mutex);
    auto it = g_opengl_preview_sessions.find(session);
    if (it != g_opengl_preview_sessions.end()) {
      opengl_state = it->second;
      g_opengl_preview_sessions.erase(it);
    }
  }

  if (!opengl_state || !load_preview_gl_functions())
    return;

  const uint64_t context_key = current_gl_context_key();
  if (context_key == 0)
    return;

  std::lock_guard capture_lock(opengl_state->mutex);
  auto context_it = opengl_state->contexts.find(context_key);
  if (context_it == opengl_state->contexts.end())
    return;

  destroy_opengl_preview_context_resources(*context_it->second);
  opengl_state->contexts.erase(context_it);
}

enum class PreviewAttachmentKind {
  Unsupported,
  Texture2D,
  Texture2DArrayLayer0,
};

static PreviewAttachmentKind
get_preview_attachment_kind(const PreviewCaptureRequest &request) {
  if (request.face_count != 1)
    return PreviewAttachmentKind::Unsupported;
  if (request.array_size > 1)
    return PreviewAttachmentKind::Texture2DArrayLayer0;
  return PreviewAttachmentKind::Texture2D;
}

static bool build_preview_capture_request(const InstanceData *data,
                                          XrSwapchain swapchain,
                                          PreviewCaptureRequest &request) {
  auto swapchain_it = data->swapchains.find(swapchain);
  if (swapchain_it == data->swapchains.end())
    return false;

  const TrackedSwapchain &tracked_swapchain = swapchain_it->second;
  request.session = tracked_swapchain.session;
  request.swapchain = swapchain;
  request.usage_flags = tracked_swapchain.usage_flags;
  request.width = tracked_swapchain.width;
  request.height = tracked_swapchain.height;
  request.array_size = tracked_swapchain.array_size;
  request.face_count = tracked_swapchain.face_count;
  request.sample_count = tracked_swapchain.sample_count;
  request.format = tracked_swapchain.format;
  request.release_serial = tracked_swapchain.release_serial;
  request.has_latest_acquired_index =
      tracked_swapchain.has_latest_acquired_index;
  if (tracked_swapchain.has_latest_acquired_index &&
      tracked_swapchain.latest_acquired_index <
          tracked_swapchain.images.size()) {
    request.image_index = tracked_swapchain.latest_acquired_index;
    request.image_type =
        tracked_swapchain.images[tracked_swapchain.latest_acquired_index].type;
    request.image_handle_value =
        tracked_swapchain.images[tracked_swapchain.latest_acquired_index]
            .handle_value;
  }

  auto session_it = data->sessions.find(tracked_swapchain.session);
  if (session_it != data->sessions.end())
    request.graphics_binding = session_it->second.graphics_binding;

  return true;
}

static const char *
preview_capture_skip_reason_opengl(const PreviewCaptureRequest &request) {
  if (request.graphics_binding != TrackedSession::GraphicsBindingKind::OPENGL)
    return "Session graphics binding is not classified as desktop OpenGL";

  if ((request.usage_flags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) == 0)
    return "Swapchain is not a color attachment";

  if (!request.has_latest_acquired_index)
    return "No acquired swapchain image is available yet";

  if (request.image_type != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR)
    return "Swapchain image type is not XrSwapchainImageOpenGLKHR";

  if (get_preview_attachment_kind(request) ==
      PreviewAttachmentKind::Unsupported)
    return "Cube and multi-face OpenGL swapchains are not supported yet";

  PreviewCaptureConfig config = get_preview_capture_config();
  if (config.capture_interval <= 0)
    return "Preview capture disabled by XR_DEBUG_GUI_PREVIEW_INTERVAL=0";

  uint64_t pending_serial = request.release_serial + 1;
  if (pending_serial != 1 &&
      pending_serial % (uint64_t)config.capture_interval != 0)
    return "Waiting for the configured preview capture interval";

  return nullptr;
}

static const char *
preview_capture_skip_reason_vulkan(const PreviewCaptureRequest &request) {
  if (request.graphics_binding != TrackedSession::GraphicsBindingKind::VULKAN)
    return "Session graphics binding is not classified as Vulkan";

  if ((request.usage_flags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) !=
          0 &&
      (request.usage_flags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) == 0)
    return "Depth/stencil Vulkan swapchains are not supported yet";

  if (!request.has_latest_acquired_index)
    return "No acquired swapchain image is available yet";

  if (request.image_type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR &&
      request.image_type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR)
    return "Swapchain image type is not a Vulkan swapchain image";

  if (request.image_handle_value == 0)
    return "Tracked Vulkan swapchain image handle is null";

  if (request.face_count != 1)
    return "Cube and multi-face Vulkan swapchains are not supported yet";

  if (request.sample_count != 1)
    return "Multisampled Vulkan swapchains are not supported yet";

  PreviewCaptureConfig config = get_preview_capture_config();
  if (config.capture_interval <= 0)
    return "Preview capture disabled by XR_DEBUG_GUI_PREVIEW_INTERVAL=0";

  uint64_t pending_serial = request.release_serial + 1;
  if (pending_serial != 1 &&
      pending_serial % (uint64_t)config.capture_interval != 0)
    return "Waiting for the configured preview capture interval";

  if ((request.usage_flags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) == 0)
    return "Vulkan preview currently requires XR_SWAPCHAIN_USAGE_SAMPLED_BIT";

  VkImageLayout inferred_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!infer_vulkan_source_layout(request, inferred_layout))
    return "Vulkan preview could not infer the source image layout";

  if (!get_vulkan_preview_session_state(request.session))
    return "Vulkan preview session state was not captured from xrCreateSession";

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

static bool capture_opengl_preview(const PreviewCaptureRequest &request,
                                   TrackedPreviewImage &preview,
                                   std::string &status) {
  if (!load_preview_gl_functions()) {
    status = "OpenGL preview helpers are unavailable on this thread";
    return false;
  }

  PreviewCaptureConfig config = get_preview_capture_config();
  if (request.width == 0 || request.height == 0) {
    status = "Swapchain extent is zero";
    return false;
  }

  uint32_t preview_width = request.width;
  uint32_t preview_height = request.height;
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
               (GLsizei)preview_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

  preview_gl.GenFramebuffers(1, &src_fbo);
  preview_gl.GenFramebuffers(1, &dst_fbo);

  preview_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
  switch (get_preview_attachment_kind(request)) {
  case PreviewAttachmentKind::Texture2D:
    preview_gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D,
                                    (GLuint)request.image_handle_value, 0);
    break;
  case PreviewAttachmentKind::Texture2DArrayLayer0:
    preview_gl.FramebufferTextureLayer(
        GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        (GLuint)request.image_handle_value, 0, 0);
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
    preview_gl.BlitFramebuffer(0, 0, (GLint)request.width,
                               (GLint)request.height, 0, 0,
                               (GLint)preview_width, (GLint)preview_height,
                               GL_COLOR_BUFFER_BIT, GL_LINEAR);

    preview_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, dst_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    preview.rgba8.resize((size_t)preview_width * (size_t)preview_height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)preview_width, (GLsizei)preview_height, GL_RGBA,
                 GL_UNSIGNED_BYTE, preview.rgba8.data());
    flip_rgba_rows(preview.rgba8, preview_width, preview_height);

    preview.available = true;
    preview.capture_serial = request.release_serial + 1;
    preview.image_index = request.image_index;
    preview.image_array_index = 0;
    preview.width = preview_width;
    preview.height = preview_height;
    status = request.array_size > 1 ? "Captured preview from array layer 0"
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

static bool ensure_opengl_preview_slot_resources(OpenGLPreviewCaptureSlot &slot,
                                                 uint32_t preview_width,
                                                 uint32_t preview_height,
                                                 std::string &status) {
  if (!load_preview_gl_functions()) {
    status = "OpenGL preview helpers are unavailable on this thread";
    return false;
  }

  if (slot.src_fbo == 0)
    preview_gl.GenFramebuffers(1, &slot.src_fbo);
  if (slot.dst_fbo == 0)
    preview_gl.GenFramebuffers(1, &slot.dst_fbo);
  if (slot.pbo == 0)
    preview_gl.GenBuffers(1, &slot.pbo);
  if (slot.dst_texture == 0)
    glGenTextures(1, &slot.dst_texture);

  if (slot.src_fbo == 0 || slot.dst_fbo == 0 || slot.pbo == 0 ||
      slot.dst_texture == 0) {
    status = "Failed to allocate OpenGL preview slot resources";
    return false;
  }

  const size_t byte_size = (size_t)preview_width * (size_t)preview_height * 4;
  if (slot.preview_width != preview_width || slot.preview_height != preview_height) {
    GLint prev_texture_2d = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_texture_2d);
    glBindTexture(GL_TEXTURE_2D, slot.dst_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)preview_width,
                 (GLsizei)preview_height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_texture_2d);

    slot.preview_width = preview_width;
    slot.preview_height = preview_height;
    slot.byte_size = byte_size;
  }

  if (slot.byte_size != byte_size)
    slot.byte_size = byte_size;

  return true;
}

static void harvest_completed_opengl_preview_captures(XrSession session,
                                                      bool wait_for_all) {
  if (!load_preview_gl_functions())
    return;

  const uint64_t context_key = current_gl_context_key();
  if (context_key == 0)
    return;

  std::shared_ptr<OpenGLPreviewSessionState> session_state =
      get_opengl_preview_session_state(session, false);
  if (!session_state)
    return;

  std::vector<OpenGLPreviewHarvestResult> completed;
  GLint prev_pixel_pack_buffer = 0;
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pixel_pack_buffer);

  {
    std::lock_guard lock(session_state->mutex);
    auto context_it = session_state->contexts.find(context_key);
    if (context_it == session_state->contexts.end())
      return;

    for (auto &slot : context_it->second->slots) {
      if (!slot.in_flight || slot.fence == nullptr)
        continue;

      GLenum wait_result = preview_gl.ClientWaitSync(
          slot.fence, wait_for_all ? GL_SYNC_FLUSH_COMMANDS_BIT : 0,
          wait_for_all ? GL_TIMEOUT_IGNORED : 0);
      if (!wait_for_all && wait_result == GL_TIMEOUT_EXPIRED)
        continue;

      OpenGLPreviewHarvestResult result;
      result.swapchain = slot.swapchain;
      int64_t finalize_begin_ns = now_ns();

      if (wait_result == GL_WAIT_FAILED) {
        result.status = "Failed to observe OpenGL preview completion fence";
      } else {
        preview_gl.BindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
        void *mapped =
            preview_gl.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                      (GLsizeiptr)slot.byte_size, GL_MAP_READ_BIT);
        if (mapped == nullptr) {
          result.status = "Failed to map the OpenGL preview PBO";
        } else {
          result.preview.rgba8.resize(slot.byte_size);
          std::memcpy(result.preview.rgba8.data(), mapped, slot.byte_size);
          preview_gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
          flip_rgba_rows(result.preview.rgba8, slot.preview_width,
                         slot.preview_height);
          result.preview.available = true;
          result.preview.capture_serial = slot.capture_serial;
          result.preview.image_index = slot.image_index;
          result.preview.image_array_index = slot.image_array_index;
          result.preview.width = slot.preview_width;
          result.preview.height = slot.preview_height;
          result.status = slot.image_array_index != 0
                              ? "Captured OpenGL preview asynchronously from array layer 0"
                              : "Captured OpenGL preview asynchronously";
        }
      }

      int64_t finalize_end_ns = now_ns();
      result.finalize_ms = ns_to_ms(finalize_begin_ns, finalize_end_ns);
      if (slot.submit_end_ns != 0)
        result.ready_latency_ms = ns_to_ms(slot.submit_end_ns, finalize_end_ns);

      if (slot.fence != nullptr)
        preview_gl.DeleteSync(slot.fence);
      slot.fence = nullptr;
      slot.in_flight = false;
      slot.swapchain = XR_NULL_HANDLE;
      slot.image_index = 0;
      slot.image_array_index = 0;
      slot.capture_serial = 0;
      slot.submit_begin_ns = 0;
      slot.submit_end_ns = 0;

      completed.push_back(std::move(result));
    }
  }

  preview_gl.BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prev_pixel_pack_buffer);

  for (auto &result : completed) {
    InstanceData *data = GetInstanceDataFromSwapchain(result.swapchain);
    if (data == nullptr)
      continue;

    std::unique_lock lock(data->state_mutex);
    auto tracked_it = data->swapchains.find(result.swapchain);
    if (tracked_it == data->swapchains.end())
      continue;

    TrackedSwapchain &tracked = tracked_it->second;
    if (tracked.preview_inflight_count > 0)
      tracked.preview_inflight_count--;

    if (result.preview.available) {
      tracked.latest_preview = result.preview;
      tracked.preview_images[make_preview_key(result.preview.image_index,
                                             result.preview.image_array_index)] =
          result.preview;
      tracked.preview_success_count++;
    }

    tracked.preview_status = result.status;
    record_preview_timing(result.finalize_ms, tracked.preview_finalize_ms_last,
                          tracked.preview_finalize_ms_max,
                          tracked.preview_finalize_ms_total,
                          tracked.preview_finalize_sample_count);
    if (result.ready_latency_ms > 0.0f) {
      record_preview_timing(result.ready_latency_ms,
                            tracked.preview_ready_latency_ms_last,
                            tracked.preview_ready_latency_ms_max,
                            tracked.preview_ready_latency_ms_total,
                            tracked.preview_ready_latency_sample_count);
    }
  }
}

static OpenGLPreviewSubmitResult
submit_opengl_preview_capture(const PreviewCaptureRequest &request) {
  OpenGLPreviewSubmitResult result;
  if (!load_preview_gl_functions()) {
    result.status = "OpenGL preview helpers are unavailable on this thread";
    return result;
  }

  const uint64_t context_key = current_gl_context_key();
  if (context_key == 0) {
    result.status = "OpenGL preview requires a current application GL context";
    return result;
  }

  std::shared_ptr<OpenGLPreviewSessionState> session_state =
      get_opengl_preview_session_state(request.session, true);
  if (!session_state) {
    result.status = "Failed to allocate OpenGL preview session state";
    return result;
  }

  PreviewCaptureConfig config = get_preview_capture_config();
  if (request.width == 0 || request.height == 0) {
    result.status = "Swapchain extent is zero";
    return result;
  }

  uint32_t preview_width = request.width;
  uint32_t preview_height = request.height;
  uint32_t longest_edge = std::max(preview_width, preview_height);
  if (longest_edge > (uint32_t)config.max_edge) {
    float scale = (float)config.max_edge / (float)longest_edge;
    preview_width = std::max(1u, (uint32_t)(preview_width * scale));
    preview_height = std::max(1u, (uint32_t)(preview_height * scale));
  }

  int64_t submit_begin_ns = now_ns();
  GLint prev_read_fbo = 0;
  GLint prev_draw_fbo = 0;
  GLint prev_texture_2d = 0;
  GLint prev_read_buffer = 0;
  GLint prev_draw_buffer = 0;
  GLint prev_pack_alignment = 0;
  GLint prev_pixel_pack_buffer = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_texture_2d);
  glGetIntegerv(GL_READ_BUFFER, &prev_read_buffer);
  glGetIntegerv(GL_DRAW_BUFFER, &prev_draw_buffer);
  glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack_alignment);
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pixel_pack_buffer);

  auto restore_gl_state = [&]() {
    preview_gl.BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prev_pixel_pack_buffer);
    preview_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read_fbo);
    preview_gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw_fbo);
    glReadBuffer(prev_read_buffer);
    glDrawBuffer(prev_draw_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, prev_pack_alignment);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_texture_2d);
  };

  do {
    std::lock_guard lock(session_state->mutex);
    OpenGLPreviewContextState *context_state =
        get_or_create_opengl_preview_context_state(*session_state, context_key);
    if (context_state == nullptr) {
      result.status = "Failed to allocate OpenGL preview context state";
      break;
    }

    OpenGLPreviewCaptureSlot *slot = nullptr;
    for (auto &candidate : context_state->slots) {
      if (!candidate.in_flight) {
        slot = &candidate;
        break;
      }
    }
    if (slot == nullptr) {
      result.status = "All OpenGL preview capture slots are busy";
      break;
    }

    if (!ensure_opengl_preview_slot_resources(*slot, preview_width,
                                              preview_height, result.status)) {
      break;
    }

    if (slot->fence != nullptr) {
      preview_gl.DeleteSync(slot->fence);
      slot->fence = nullptr;
    }

    preview_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, slot->src_fbo);
    switch (get_preview_attachment_kind(request)) {
    case PreviewAttachmentKind::Texture2D:
      preview_gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                      GL_TEXTURE_2D,
                                      (GLuint)request.image_handle_value, 0);
      break;
    case PreviewAttachmentKind::Texture2DArrayLayer0:
      preview_gl.FramebufferTextureLayer(GL_READ_FRAMEBUFFER,
                                         GL_COLOR_ATTACHMENT0,
                                         (GLuint)request.image_handle_value, 0,
                                         0);
      break;
    case PreviewAttachmentKind::Unsupported:
      result.status = "OpenGL preview attachment kind is unsupported";
      break;
    }
    if (!result.status.empty())
      break;

    preview_gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, slot->dst_fbo);
    preview_gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, slot->dst_texture, 0);

    GLenum read_status = preview_gl.CheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    GLenum draw_status = preview_gl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (read_status != GL_FRAMEBUFFER_COMPLETE ||
        draw_status != GL_FRAMEBUFFER_COMPLETE) {
      std::ostringstream oss;
      oss << "Framebuffer incomplete (read=0x" << std::hex << read_status
          << ", draw=0x" << draw_status << ")";
      result.status = oss.str();
      break;
    }

    preview_gl.BindBuffer(GL_PIXEL_PACK_BUFFER, slot->pbo);
    preview_gl.BufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)slot->byte_size,
                          nullptr, GL_STREAM_READ);

    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    preview_gl.BlitFramebuffer(0, 0, (GLint)request.width, (GLint)request.height,
                               0, 0, (GLint)preview_width,
                               (GLint)preview_height, GL_COLOR_BUFFER_BIT,
                               GL_LINEAR);

    preview_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, slot->dst_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)preview_width, (GLsizei)preview_height,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    slot->fence = preview_gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (slot->fence == nullptr) {
      result.status = "Failed to create the OpenGL preview fence";
      break;
    }
    glFlush();

    int64_t submit_end_ns = now_ns();
    slot->in_flight = true;
    slot->swapchain = request.swapchain;
    slot->image_index = request.image_index;
    slot->image_array_index = 0;
    slot->capture_serial = request.release_serial + 1;
    slot->submit_begin_ns = submit_begin_ns;
    slot->submit_end_ns = submit_end_ns;

    result.submitted = true;
    result.app_thread_ms = ns_to_ms(submit_begin_ns, submit_end_ns);
    result.status = request.array_size > 1
                        ? "Submitted OpenGL preview capture from array layer 0"
                        : "Submitted OpenGL preview capture";
  } while (false);

  restore_gl_state();
  return result;
}

static void harvest_completed_vulkan_preview_captures(XrSession session,
                                                      bool wait_for_all) {
  std::shared_ptr<VulkanPreviewSessionState> state =
      get_vulkan_preview_session_state(session);
  if (!state)
    return;

  std::vector<VulkanPreviewHarvestResult> completed;
  {
    std::lock_guard capture_lock(state->mutex);
    VkDevice device = state->binding.device;
    if (device == VK_NULL_HANDLE)
      return;

    for (auto &slot : state->slots) {
      if (!slot.in_flight || slot.fence == VK_NULL_HANDLE)
        continue;

      VkResult fence_result = wait_for_all
                                  ? vkWaitForFences(device, 1, &slot.fence,
                                                    VK_TRUE, UINT64_MAX)
                                  : vkGetFenceStatus(device, slot.fence);
      if (!wait_for_all && fence_result == VK_NOT_READY)
        continue;

      VulkanPreviewHarvestResult result;
      result.swapchain = slot.swapchain;
      int64_t finalize_begin_ns = now_ns();

      if (fence_result != VK_SUCCESS) {
        result.status = "Failed to observe Vulkan preview completion fence";
      } else {
        if (!slot.staging_memory_coherent) {
          VkMappedMemoryRange mapped_range{
              VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
          mapped_range.memory = slot.staging_memory;
          mapped_range.offset = 0;
          mapped_range.size = slot.staging_buffer_size;
          if (vkInvalidateMappedMemoryRanges(device, 1, &mapped_range) !=
              VK_SUCCESS) {
            result.status =
                "Failed to invalidate the Vulkan preview staging memory";
          }
        }

        if (result.status.empty()) {
          result.preview.rgba8.resize((size_t)slot.preview_width *
                                      (size_t)slot.preview_height * 4);
          std::memcpy(result.preview.rgba8.data(), slot.mapped_staging,
                      result.preview.rgba8.size());
          result.preview.available = true;
          result.preview.capture_serial = slot.capture_serial;
          result.preview.image_index = slot.image_index;
          result.preview.image_array_index = slot.image_array_index;
          result.preview.width = slot.preview_width;
          result.preview.height = slot.preview_height;
          result.status = slot.image_array_index != 0
                              ? "Captured Vulkan preview asynchronously"
                              : "Captured Vulkan preview asynchronously";
        }
      }

      int64_t finalize_end_ns = now_ns();
      result.finalize_ms = ns_to_ms(finalize_begin_ns, finalize_end_ns);
      if (slot.submit_end_ns != 0)
        result.ready_latency_ms = ns_to_ms(slot.submit_end_ns, finalize_end_ns);

      destroy_vulkan_preview_slot_submission_resources(device, slot);
      completed.push_back(std::move(result));
    }
  }

  for (auto &result : completed) {
    InstanceData *data = GetInstanceDataFromSwapchain(result.swapchain);
    if (data == nullptr)
      continue;

    std::unique_lock lock(data->state_mutex);
    auto tracked_it = data->swapchains.find(result.swapchain);
    if (tracked_it == data->swapchains.end())
      continue;

    TrackedSwapchain &tracked = tracked_it->second;
    if (tracked.preview_inflight_count > 0)
      tracked.preview_inflight_count--;

    if (result.preview.available) {
      tracked.latest_preview = result.preview;
      tracked.preview_images[make_preview_key(result.preview.image_index,
                                             result.preview.image_array_index)] =
          result.preview;
      tracked.preview_success_count++;
    }

    tracked.preview_status = result.status;
    record_preview_timing(result.finalize_ms, tracked.preview_finalize_ms_last,
                          tracked.preview_finalize_ms_max,
                          tracked.preview_finalize_ms_total,
                          tracked.preview_finalize_sample_count);
    if (result.ready_latency_ms > 0.0f) {
      record_preview_timing(result.ready_latency_ms,
                            tracked.preview_ready_latency_ms_last,
                            tracked.preview_ready_latency_ms_max,
                            tracked.preview_ready_latency_ms_total,
                            tracked.preview_ready_latency_sample_count);
    }
  }
}

static VulkanPreviewSubmitResult
submit_vulkan_preview_capture(const PreviewCaptureRequest &request) {
  VulkanPreviewSubmitResult result;
  std::shared_ptr<VulkanPreviewSessionState> state =
      get_vulkan_preview_session_state(request.session);
  if (!state) {
    result.status =
        "Vulkan preview session state was not captured from xrCreateSession";
    return result;
  }

  int64_t submit_begin_ns = now_ns();
  std::lock_guard capture_lock(state->mutex);
  if (!ensure_vulkan_preview_static_resources(*state, result.status))
    return result;

  PreviewCaptureConfig config = get_preview_capture_config();
  if (request.width == 0 || request.height == 0) {
    result.status = "Swapchain extent is zero";
    return result;
  }

  uint32_t preview_width = request.width;
  uint32_t preview_height = request.height;
  uint32_t longest_edge = std::max(preview_width, preview_height);
  if (longest_edge > (uint32_t)config.max_edge) {
    float scale = (float)config.max_edge / (float)longest_edge;
    preview_width = std::max(1u, (uint32_t)(preview_width * scale));
    preview_height = std::max(1u, (uint32_t)(preview_height * scale));
  }

  VulkanPreviewCaptureSlot *slot = nullptr;
  for (auto &candidate : state->slots) {
    if (!candidate.in_flight) {
      slot = &candidate;
      break;
    }
  }
  if (slot == nullptr) {
    result.status = "All Vulkan preview capture slots are busy";
    return result;
  }

  bool recreated_target = false;
  if (!ensure_vulkan_preview_target_resources(*state, *slot, preview_width,
                                              preview_height, recreated_target,
                                              result.status)) {
    return result;
  }

  VkImage source_image = unpack_handle<VkImage>(request.image_handle_value);
  if (source_image == VK_NULL_HANDLE) {
    result.status = "Tracked Vulkan swapchain image handle is null";
    return result;
  }

  VkImageLayout source_original_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!infer_vulkan_source_layout(request, source_original_layout)) {
    result.status = "Vulkan preview could not infer the source image layout";
    return result;
  }

  VkDevice device = state->binding.device;
  destroy_vulkan_preview_slot_submission_resources(device, *slot);

  VkImageViewCreateInfo source_image_view_info{
      VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  source_image_view_info.image = source_image;
  source_image_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  source_image_view_info.format = (VkFormat)request.format;
  source_image_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  source_image_view_info.subresourceRange.baseMipLevel = 0;
  source_image_view_info.subresourceRange.levelCount = 1;
  source_image_view_info.subresourceRange.baseArrayLayer = 0;
  source_image_view_info.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device, &source_image_view_info, nullptr,
                        &slot->source_image_view) != VK_SUCCESS) {
    result.status = "Failed to create the Vulkan preview source image view";
    destroy_vulkan_preview_slot_submission_resources(device, *slot);
    return result;
  }

  VkDescriptorImageInfo descriptor_image_info{};
  descriptor_image_info.sampler = state->sampler;
  descriptor_image_info.imageView = slot->source_image_view;
  descriptor_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkWriteDescriptorSet descriptor_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  descriptor_write.dstSet = slot->descriptor_set;
  descriptor_write.dstBinding = 0;
  descriptor_write.descriptorCount = 1;
  descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  descriptor_write.pImageInfo = &descriptor_image_info;
  vkUpdateDescriptorSets(device, 1, &descriptor_write, 0, nullptr);

  if (vkResetFences(device, 1, &slot->fence) != VK_SUCCESS) {
    result.status = "Failed to reset the Vulkan preview fence";
    destroy_vulkan_preview_slot_submission_resources(device, *slot);
    return result;
  }
  if (vkResetCommandBuffer(slot->command_buffer, 0) != VK_SUCCESS) {
    result.status = "Failed to reset the Vulkan preview command buffer";
    destroy_vulkan_preview_slot_submission_resources(device, *slot);
    return result;
  }

  VkCommandBufferBeginInfo begin_info{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(slot->command_buffer, &begin_info) != VK_SUCCESS) {
    result.status =
        "Failed to begin recording the Vulkan preview command buffer";
    destroy_vulkan_preview_slot_submission_resources(device, *slot);
    return result;
  }

  VkImageMemoryBarrier source_to_shader_read{
      VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  source_to_shader_read.srcAccessMask =
      VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
  source_to_shader_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  source_to_shader_read.oldLayout = source_original_layout;
  source_to_shader_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  source_to_shader_read.image = source_image;
  source_to_shader_read.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  source_to_shader_read.subresourceRange.baseMipLevel = 0;
  source_to_shader_read.subresourceRange.levelCount = 1;
  source_to_shader_read.subresourceRange.baseArrayLayer = 0;
  source_to_shader_read.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(slot->command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &source_to_shader_read);

  VkImageMemoryBarrier destination_to_color_attachment{
      VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  destination_to_color_attachment.srcAccessMask =
      recreated_target ? 0 : VK_ACCESS_TRANSFER_READ_BIT;
  destination_to_color_attachment.dstAccessMask =
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  destination_to_color_attachment.oldLayout =
      recreated_target ? VK_IMAGE_LAYOUT_UNDEFINED
                       : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  destination_to_color_attachment.newLayout =
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  destination_to_color_attachment.image = slot->destination_image;
  destination_to_color_attachment.subresourceRange.aspectMask =
      VK_IMAGE_ASPECT_COLOR_BIT;
  destination_to_color_attachment.subresourceRange.baseMipLevel = 0;
  destination_to_color_attachment.subresourceRange.levelCount = 1;
  destination_to_color_attachment.subresourceRange.baseArrayLayer = 0;
  destination_to_color_attachment.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(slot->command_buffer,
                       recreated_target ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                        : VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                       nullptr, 0, nullptr, 1,
                       &destination_to_color_attachment);

  VkClearValue clear_value{};
  VkRenderPassBeginInfo render_pass_begin_info{
      VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  render_pass_begin_info.renderPass = state->render_pass;
  render_pass_begin_info.framebuffer = slot->framebuffer;
  render_pass_begin_info.renderArea.extent = {preview_width, preview_height};
  render_pass_begin_info.clearValueCount = 1;
  render_pass_begin_info.pClearValues = &clear_value;

  vkCmdBeginRenderPass(slot->command_buffer, &render_pass_begin_info,
                       VK_SUBPASS_CONTENTS_INLINE);

  VkViewport viewport{};
  viewport.width = (float)preview_width;
  viewport.height = (float)preview_height;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(slot->command_buffer, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.extent = {preview_width, preview_height};
  vkCmdSetScissor(slot->command_buffer, 0, 1, &scissor);

  vkCmdBindPipeline(slot->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    state->pipeline);
  vkCmdBindDescriptorSets(slot->command_buffer,
                          VK_PIPELINE_BIND_POINT_GRAPHICS,
                          state->pipeline_layout, 0, 1, &slot->descriptor_set,
                          0, nullptr);
  vkCmdDraw(slot->command_buffer, 3, 1, 0, 0);
  vkCmdEndRenderPass(slot->command_buffer);

  VkImageMemoryBarrier destination_to_transfer_src{
      VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  destination_to_transfer_src.srcAccessMask =
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  destination_to_transfer_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  destination_to_transfer_src.oldLayout =
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  destination_to_transfer_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  destination_to_transfer_src.image = slot->destination_image;
  destination_to_transfer_src.subresourceRange.aspectMask =
      VK_IMAGE_ASPECT_COLOR_BIT;
  destination_to_transfer_src.subresourceRange.baseMipLevel = 0;
  destination_to_transfer_src.subresourceRange.levelCount = 1;
  destination_to_transfer_src.subresourceRange.baseArrayLayer = 0;
  destination_to_transfer_src.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(slot->command_buffer,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &destination_to_transfer_src);

  VkImageMemoryBarrier source_restore{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  source_restore.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  source_restore.dstAccessMask = 0;
  source_restore.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  source_restore.newLayout = source_original_layout;
  source_restore.image = source_image;
  source_restore.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  source_restore.subresourceRange.baseMipLevel = 0;
  source_restore.subresourceRange.levelCount = 1;
  source_restore.subresourceRange.baseArrayLayer = 0;
  source_restore.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(slot->command_buffer,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &source_restore);

  VkBufferImageCopy copy_region{};
  copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy_region.imageSubresource.mipLevel = 0;
  copy_region.imageSubresource.baseArrayLayer = 0;
  copy_region.imageSubresource.layerCount = 1;
  copy_region.imageExtent = {preview_width, preview_height, 1};
  vkCmdCopyImageToBuffer(slot->command_buffer, slot->destination_image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         slot->staging_buffer, 1, &copy_region);

  VkBufferMemoryBarrier buffer_to_host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  buffer_to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  buffer_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  buffer_to_host.buffer = slot->staging_buffer;
  buffer_to_host.offset = 0;
  buffer_to_host.size = slot->staging_buffer_size;
  vkCmdPipelineBarrier(slot->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                       &buffer_to_host, 0, nullptr);

  if (vkEndCommandBuffer(slot->command_buffer) != VK_SUCCESS) {
    result.status = "Failed to end the Vulkan preview command buffer";
    destroy_vulkan_preview_slot_submission_resources(device, *slot);
    return result;
  }

  VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &slot->command_buffer;
  if (vkQueueSubmit(state->queue, 1, &submit_info, slot->fence) !=
      VK_SUCCESS) {
    result.status = "Failed to submit the Vulkan preview commands";
    destroy_vulkan_preview_slot_submission_resources(device, *slot);
    return result;
  }

  int64_t submit_end_ns = now_ns();
  slot->in_flight = true;
  slot->swapchain = request.swapchain;
  slot->image_index = request.image_index;
  slot->image_array_index = 0;
  slot->capture_serial = request.release_serial + 1;
  slot->preview_width = preview_width;
  slot->preview_height = preview_height;
  slot->submit_begin_ns = submit_begin_ns;
  slot->submit_end_ns = submit_end_ns;

  result.submitted = true;
  result.app_thread_ms = ns_to_ms(submit_begin_ns, submit_end_ns);
  result.status = request.array_size > 1
                      ? "Submitted Vulkan preview capture from array layer 0"
                      : "Submitted Vulkan preview capture";
  return result;
}

static TrackedSwapchainImage
track_swapchain_image(const XrSwapchainImageBaseHeader &image) {
  TrackedSwapchainImage tracked;
  tracked.type = image.type;

  if (image.type == XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR) {
    auto &gl_image = reinterpret_cast<const XrSwapchainImageOpenGLKHR &>(image);
    tracked.handle_value = (uint64_t)gl_image.image;
  } else if (image.type == XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR ||
             image.type == XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR) {
    auto &vk_image = reinterpret_cast<const XrSwapchainImageVulkanKHR &>(image);
    tracked.handle_value = pack_handle(vk_image.image);
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
  if (XR_SUCCEEDED(result) && images != nullptr && imageCountOutput != nullptr) {
    std::unique_lock lock(data->state_mutex);
    auto it = data->swapchains.find(swapchain);
    if (it != data->swapchains.end()) {
      it->second.images.clear();
      it->second.images.reserve(*imageCountOutput);
      if (*imageCountOutput == 0)
        return result;

      switch (images[0].type) {
      case XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR: {
        auto *typed_images =
            reinterpret_cast<const XrSwapchainImageOpenGLKHR *>(images);
        for (uint32_t image_index = 0; image_index < *imageCountOutput;
             ++image_index) {
          it->second.images.push_back(track_swapchain_image(
              reinterpret_cast<const XrSwapchainImageBaseHeader &>(
                  typed_images[image_index])));
        }
        break;
      }
      case XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR: {
        auto *typed_images =
            reinterpret_cast<const XrSwapchainImageVulkanKHR *>(images);
        for (uint32_t image_index = 0; image_index < *imageCountOutput;
             ++image_index) {
          it->second.images.push_back(track_swapchain_image(
              reinterpret_cast<const XrSwapchainImageBaseHeader &>(
                  typed_images[image_index])));
        }
        break;
      }
      default: {
        TrackedSwapchainImage tracked;
        tracked.type = images[0].type;
        for (uint32_t image_index = 0; image_index < *imageCountOutput;
             ++image_index)
          it->second.images.push_back(tracked);
        break;
      }
      }
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

  PreviewCaptureRequest request;
  TrackedPreviewImage captured_preview;
  std::string preview_status;
  bool has_captured_preview = false;
  bool attempted_preview_capture = false;
  bool submitted_async_preview = false;
  float preview_app_thread_ms = 0.0f;
  bool have_request = false;
  {
    std::shared_lock lock(data->state_mutex);
    have_request = build_preview_capture_request(data, swapchain, request);
  }

  if (have_request) {
    switch (request.graphics_binding) {
    case TrackedSession::GraphicsBindingKind::OPENGL:
      harvest_completed_opengl_preview_captures(request.session, false);
      break;
    case TrackedSession::GraphicsBindingKind::VULKAN:
      harvest_completed_vulkan_preview_captures(request.session, false);
      break;
    default:
      break;
    }
  }

  if (have_request) {
    const char *skip_reason = nullptr;
    switch (request.graphics_binding) {
    case TrackedSession::GraphicsBindingKind::OPENGL:
      skip_reason = preview_capture_skip_reason_opengl(request);
      if (skip_reason == nullptr) {
        OpenGLPreviewSubmitResult submit_result =
            submit_opengl_preview_capture(request);
        attempted_preview_capture = submit_result.submitted;
        submitted_async_preview = submit_result.submitted;
        preview_app_thread_ms = submit_result.app_thread_ms;
        preview_status = submit_result.status;
      }
      break;
    case TrackedSession::GraphicsBindingKind::VULKAN:
      skip_reason = preview_capture_skip_reason_vulkan(request);
      if (skip_reason == nullptr) {
        VulkanPreviewSubmitResult submit_result =
            submit_vulkan_preview_capture(request);
        attempted_preview_capture = submit_result.submitted;
        submitted_async_preview = submit_result.submitted;
        preview_app_thread_ms = submit_result.app_thread_ms;
        preview_status = submit_result.status;
      }
      break;
    default:
      skip_reason =
          "Session graphics binding is not supported for preview capture";
      break;
    }

    if (skip_reason != nullptr)
      preview_status = skip_reason;
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
        tracked.preview_images[make_preview_key(
            tracked.latest_preview.image_index,
            tracked.latest_preview.image_array_index)] = tracked.latest_preview;
      }

      if (attempted_preview_capture) {
        tracked.preview_attempt_count++;
        if (has_captured_preview)
          tracked.preview_success_count++;
        if (preview_app_thread_ms > 0.0f) {
          record_preview_timing(
              preview_app_thread_ms, tracked.preview_app_thread_ms_last,
              tracked.preview_app_thread_ms_max,
              tracked.preview_app_thread_ms_total,
              tracked.preview_app_thread_sample_count);
        }
        if (submitted_async_preview) {
          tracked.preview_inflight_count++;
          tracked.preview_inflight_peak =
              std::max(tracked.preview_inflight_peak,
                       tracked.preview_inflight_count);
        }
      } else {
        tracked.preview_skip_count++;
      }

      if (!preview_status.empty() || has_captured_preview ||
          attempted_preview_capture || !had_preview) {
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
          << " submitted=" << tracked.preview_attempt_count
          << " completed=" << tracked.preview_success_count
          << " no_submit=" << tracked.preview_skip_count
            << " inflight=" << tracked.preview_inflight_count << "/"
            << tracked.preview_inflight_peak
            << " app_ms=" << tracked.preview_app_thread_ms_last
            << " finalize_ms=" << tracked.preview_finalize_ms_last
            << " ready_ms=" << tracked.preview_ready_latency_ms_last
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

  if (have_request) {
    switch (request.graphics_binding) {
    case TrackedSession::GraphicsBindingKind::OPENGL:
      harvest_completed_opengl_preview_captures(request.session, false);
      break;
    case TrackedSession::GraphicsBindingKind::VULKAN:
      harvest_completed_vulkan_preview_captures(request.session, false);
      break;
    default:
      break;
    }
  }
  return result;
}

} // namespace debug_layer
