// SPDX-License-Identifier: MIT
// gui_preview_cache.h — Shared GUI-side cache for uploaded swapchain previews.
#pragma once

#include "../tracked_state.h"

#include <SDL3/SDL_opengl.h>

#include <cstddef>
#include <cstdint>
#include <unordered_set>

namespace debug_layer {

struct PreviewTextureKey {
  uint64_t swapchain = 0;
  uint32_t image_index = 0;
  uint32_t image_array_index = 0;

  bool operator==(const PreviewTextureKey &other) const {
    return swapchain == other.swapchain && image_index == other.image_index &&
           image_array_index == other.image_array_index;
  }
};

struct PreviewTextureKeyHash {
  size_t operator()(const PreviewTextureKey &key) const;
};

struct PreviewLookupResult {
  const TrackedPreviewImage *preview = nullptr;
  PreviewTextureKey texture_key = {};
};

PreviewLookupResult
gui_lookup_preview_for_sub_image(const TrackedSwapchain &swapchain,
                                 const TrackedCompositionSubImage &sub_image);

GLuint gui_ensure_preview_texture(const PreviewTextureKey &cache_key,
                                  const TrackedPreviewImage &preview);

GLuint gui_lookup_cached_preview_texture(const PreviewTextureKey &cache_key);

void gui_prune_preview_textures(
  const std::unordered_set<uint64_t> &live_swapchains);

} // namespace debug_layer
