// SPDX-License-Identifier: MIT
// gui_preview_cache.cpp — Shared GUI-side cache for uploaded swapchain previews.

#include "gui_preview_cache.h"

#include <SDL3/SDL_opengl.h>

#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace debug_layer {

namespace {

uint64_t make_preview_key(uint32_t image_index, uint32_t image_array_index) {
  return ((uint64_t)image_array_index << 32) | (uint64_t)image_index;
}

struct GuiPreviewTexture {
  GLuint texture = 0;
  uint64_t capture_serial = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

std::unordered_map<PreviewTextureKey, GuiPreviewTexture, PreviewTextureKeyHash>
    g_preview_textures;

} // namespace

size_t PreviewTextureKeyHash::operator()(const PreviewTextureKey &key) const {
  size_t hash = std::hash<uint64_t>{}(key.swapchain);
  hash ^= std::hash<uint32_t>{}(key.image_index) + 0x9e3779b9 + (hash << 6) +
          (hash >> 2);
  hash ^= std::hash<uint32_t>{}(key.image_array_index) + 0x9e3779b9 +
          (hash << 6) + (hash >> 2);
  return hash;
}

PreviewLookupResult
gui_lookup_preview_for_sub_image(const TrackedSwapchain &swapchain,
                                 const TrackedCompositionSubImage &sub_image) {
  PreviewLookupResult result;

  if (sub_image.has_image_index) {
    auto preview_it = swapchain.preview_images.find(
        make_preview_key(sub_image.image_index, sub_image.image_array_index));
    if (preview_it != swapchain.preview_images.end() &&
        preview_it->second.available) {
      result.preview = &preview_it->second;
      result.texture_key = {(uint64_t)swapchain.handle, preview_it->second.image_index,
                            preview_it->second.image_array_index};
      return result;
    }
  }

  if (swapchain.latest_preview.available) {
    result.preview = &swapchain.latest_preview;
    result.texture_key = {(uint64_t)swapchain.handle,
                          swapchain.latest_preview.image_index,
                          swapchain.latest_preview.image_array_index};
  }

  return result;
}

GLuint gui_ensure_preview_texture(const PreviewTextureKey &cache_key,
                                  const TrackedPreviewImage &preview) {
  GuiPreviewTexture &entry = g_preview_textures[cache_key];
  if (entry.texture == 0)
    glGenTextures(1, &entry.texture);

  if (entry.capture_serial != preview.capture_serial ||
      entry.width != preview.width || entry.height != preview.height) {
    glBindTexture(GL_TEXTURE_2D, entry.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)preview.width,
                 (GLsizei)preview.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 preview.rgba8.data());

    entry.capture_serial = preview.capture_serial;
    entry.width = preview.width;
    entry.height = preview.height;
  }

  return entry.texture;
}

GLuint gui_lookup_cached_preview_texture(const PreviewTextureKey &cache_key) {
  auto it = g_preview_textures.find(cache_key);
  return it != g_preview_textures.end() ? it->second.texture : 0;
}

void gui_prune_preview_textures(
    const std::unordered_set<uint64_t> &live_swapchains) {
  for (auto it = g_preview_textures.begin(); it != g_preview_textures.end();) {
    if (live_swapchains.find(it->first.swapchain) == live_swapchains.end()) {
      if (it->second.texture != 0)
        glDeleteTextures(1, &it->second.texture);
      it = g_preview_textures.erase(it);
    } else {
      ++it;
    }
  }
}

} // namespace debug_layer
