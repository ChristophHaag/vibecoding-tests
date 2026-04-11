// SPDX-License-Identifier: MIT
// gui/gui_main.cpp — SDL3 + Dear ImGui debug window on a dedicated thread.

#include "gui_main.h"
#include "../instance_data.h"
#include "gui_actions.h"
#include "gui_layers.h"
#include "gui_perf.h"
#include "gui_spaces.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>
#include <openxr/openxr_reflection.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace debug_layer {

// Strip "XR_SESSION_STATE_" prefix (17 chars) from the enum name.
static const char *session_state_to_str(XrSessionState s) {
#define CASE(name, val)                                                        \
  case name:                                                                   \
    return #name + 17;
  switch (s) { XR_LIST_ENUM_XrSessionState(CASE) default : return "(unknown)"; }
#undef CASE
}

static void gui_render_thread_func(InstanceData *data) {
  // ── Read configuration ───────────────────────────────────────────────
  int target_fps = 30;
  const char *fps_env = std::getenv("XR_DEBUG_GUI_FPS");
  if (fps_env != nullptr) {
    int v = std::atoi(fps_env);
    if (v > 0 && v <= 240)
      target_fps = v;
  }
  auto frame_duration = std::chrono::milliseconds(1000 / target_fps);

  // ── Initialize SDL entirely on this thread ───────────────────────────
  // We MUST NOT init SDL or create GL contexts on the app's thread, because
  // the app may have its own EGL/GLX context current there. Using a
  // separate thread with its own SDL init avoids glXMakeCurrent conflicts.
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::cerr << "[XR_APILAYER_DEBUG_gui] SDL_Init(VIDEO) failed: "
              << SDL_GetError() << std::endl;
    data->gui_running = false;
    return;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);

  SDL_Window *window = SDL_CreateWindow(
      "OpenXR Debug GUI", 1280, 800, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (window == nullptr) {
    std::cerr << "[XR_APILAYER_DEBUG_gui] SDL_CreateWindow failed: "
              << SDL_GetError() << std::endl;
    data->gui_running = false;
    return;
  }

  SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
  if (gl_ctx == nullptr) {
    std::cerr << "[XR_APILAYER_DEBUG_gui] SDL_GL_CreateContext failed: "
              << SDL_GetError() << std::endl;
    SDL_DestroyWindow(window);
    data->gui_running = false;
    return;
  }

  SDL_GL_SetSwapInterval(0);

  // ── Initialize Dear ImGui ────────────────────────────────────────────
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

  // Persist layout between runs.  Use XDG_CONFIG_HOME or fallback to ~/.config.
  static std::string ini_path;
  {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    if (xdg && xdg[0])
      ini_path = std::string(xdg) + "/openxr_debug_gui/imgui.ini";
    else if (home && home[0])
      ini_path = std::string(home) + "/.config/openxr_debug_gui/imgui.ini";
    else
      ini_path = "/tmp/openxr_debug_gui_imgui.ini";

    // Ensure parent directory exists
    auto slash = ini_path.rfind('/');
    if (slash != std::string::npos) {
      std::string dir = ini_path.substr(0, slash);
      // mkdir -p equivalent (we only need one level)
      SDL_CreateDirectory(dir.c_str());
    }
  }
  io.IniFilename = ini_path.c_str();

  ImGui::StyleColorsDark();
  ImGuiStyle &style = ImGui::GetStyle();
  style.FrameRounding = 4.0f;
  style.GrabRounding = 4.0f;
  style.WindowRounding = 6.0f;

  ImGui_ImplSDL3_InitForOpenGL(window, gl_ctx);
  ImGui_ImplOpenGL3_Init("#version 330");

  // ── Compute CLOCK_MONOTONIC → CLOCK_REALTIME offset (once) ───────────
  // OpenXR times on Linux/Monado are CLOCK_MONOTONIC nanoseconds.
  // We capture both clocks close together to produce a stable offset so the
  // predicted display time can be rendered as a wall-clock time.
  int64_t mono_to_real_offset_ns = 0;
  {
    struct timespec mono, real;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    int64_t mono_ns = (int64_t)mono.tv_sec * 1000000000LL + mono.tv_nsec;
    int64_t real_ns = (int64_t)real.tv_sec * 1000000000LL + real.tv_nsec;
    mono_to_real_offset_ns = real_ns - mono_ns;
  }

  // ── Set up first-run default layout ──────────────────────────────────
  // Only runs when imgui.ini doesn't exist yet.
  bool first_run = !SDL_GetPathInfo(ini_path.c_str(), nullptr);
  bool want_initial_layout = first_run;

  std::cerr << "[XR_APILAYER_DEBUG_gui] GUI render thread started."
            << std::endl;

  // ── Main loop ────────────────────────────────────────────────────────
  while (data->gui_running.load()) {
    auto frame_start = std::chrono::steady_clock::now();

    // Poll SDL events
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT) {
        data->gui_running = false;
        break;
      }
      if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        data->gui_running = false;
        break;
      }
    }

    if (!data->gui_running.load())
      break;

    // New frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // Dockspace over the entire window
    ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(
        0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_None);
    ImGuiID info_dock_id = dockspace_id;

    // ── First-run layout ─────────────────────────────────────────
    // Simple 2-column split: 3D left (60%), all info panels tabbed right (40%).
    // Users can freely drag tabs out of either dock node.
    if (want_initial_layout) {
      want_initial_layout = false;

      ImGui::DockBuilderRemoveNode(dockspace_id);
      ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
      int win_w, win_h;
      SDL_GetWindowSizeInPixels(window, &win_w, &win_h);
      ImGui::DockBuilderSetNodeSize(dockspace_id,
                                    ImVec2((float)win_w, (float)win_h));

      ImGuiID left_id, right_id;
      ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.60f, &left_id,
                                  &right_id);
      info_dock_id = right_id;

      ImGui::DockBuilderDockWindow("3D Spaces", left_id);
      // All info panels land in the right node as tabs — drag any out for
      // side-by-side.
      ImGui::DockBuilderDockWindow("Performance", right_id);
      ImGui::DockBuilderDockWindow("Composition Layers", right_id);
      ImGui::DockBuilderDockWindow("Frame Info", right_id);
      ImGui::DockBuilderDockWindow("Active Profiles & Live State", right_id);
      ImGui::DockBuilderDockWindow("Action Sets & Actions", right_id);
      ImGui::DockBuilderDockWindow("Suggested Bindings", right_id);

      ImGui::DockBuilderFinish(dockspace_id);
    }

    // ── Render panels ────────────────────────────────────────────
    gui_render_actions_panel(data);

    ImGuiWindow *layers_window = ImGui::FindWindowByName("Composition Layers");
    if (layers_window == nullptr || layers_window->DockId == 0)
      ImGui::SetNextWindowDockID(info_dock_id, ImGuiCond_Appearing);
    gui_render_layers_panel(data);

    gui_render_spaces_panel(data);
    gui_render_perf_panel(data);

    // ── Render frame info panel ───────────────────────────────────
    {
      // Local statics for graph settings (survive across frames).
      static float graph_seconds = 10.0f;
      static std::vector<float> plot_buf;

      std::shared_lock lock(data->state_mutex);
      ImGui::Begin("Frame Info");

      // Frame counter
      ImGui::Text("Frame #%llu",
                  (unsigned long long)data->frame_state.frame_count);

      // Predicted display time: raw ns + human-readable wall clock
      XrTime xr_time = data->frame_state.predicted_display_time;
      ImGui::Text("Predicted display: %lld ns", (long long)xr_time);
      if (xr_time != 0) {
        int64_t wall_ns = xr_time + mono_to_real_offset_ns;
        int64_t wall_sec = wall_ns / 1000000000LL;
        int ms = (int)((wall_ns % 1000000000LL) / 1000000LL);
        if (ms < 0) {
          wall_sec--;
          ms += 1000;
        }
        time_t t = (time_t)wall_sec;
        struct tm tm_info;
        localtime_r(&t, &tm_info);
        ImGui::Text("           → %02d:%02d:%02d.%03d", tm_info.tm_hour,
                    tm_info.tm_min, tm_info.tm_sec, ms);
      }

      // Refresh rate
      float period_ms = 11.11f; // fallback ~90 Hz
      if (data->frame_state.predicted_display_period > 0) {
        period_ms = (float)(data->frame_state.predicted_display_period * 1e-6);
        double fps = 1e9 / (double)data->frame_state.predicted_display_period;
        ImGui::Text("Target refresh: %.1f Hz (%.3f ms)", fps, period_ms);
      }
      ImGui::Text("Should render: %s",
                  data->frame_state.should_render ? "true" : "false");

      // Session states with human-readable enum names
      for (auto &[handle, s] : data->sessions) {
        ImGui::Text("Session: %s", session_state_to_str(s.state));
      }

      ImGui::Separator();

      // ── Frame timing graph ─────────────────────────────────
      ImGui::Text("Frame timing (delta between consecutive predicted times)");
      ImGui::SetNextItemWidth(200.0f);
      ImGui::SliderFloat("Span##graph", &graph_seconds, 1.0f, 60.0f, "%.0f s");

      size_t total = data->frame_state.timing_count;
      if (total > 0) {
        // How many samples span the requested time window?
        int samples_for_span = (period_ms > 0.0f)
                                   ? (int)(graph_seconds * 1000.0f / period_ms)
                                   : (int)total;
        int show = (int)std::min((size_t)samples_for_span, total);
        show = std::max(show, 1);

        // Extract `show` most-recent samples from ring buffer into plot_buf.
        plot_buf.resize((size_t)show);
        size_t kMax = TrackedFrameState::kMaxTimingSamples;
        size_t write_idx = data->frame_state.timing_write_idx;
        // Oldest of the `show` samples we want:
        size_t start = (write_idx + kMax - (size_t)show) % kMax;
        for (int i = 0; i < show; ++i)
          plot_buf[i] = data->frame_state.timing_deltas_ms[(start + i) % kMax];

        char overlay[64];
        snprintf(overlay, sizeof(overlay), "period %.2f ms", period_ms);
        // Auto-scale to data; overlay shows nominal period for reference.
        ImGui::PlotLines("##timing", plot_buf.data(), show, 0, overlay, FLT_MAX,
                         FLT_MAX, ImVec2(-1.0f, 100.0f));
      } else {
        ImGui::TextDisabled("(waiting for frames…)");
      }

      ImGui::End();
    }

    // Finalize
    ImGui::Render();

    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.10f, 0.10f, 0.12f, 1.00f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    SDL_GL_SwapWindow(window);

    // Frame pacing
    auto frame_end = std::chrono::steady_clock::now();
    auto elapsed = frame_end - frame_start;
    if (elapsed < frame_duration)
      std::this_thread::sleep_for(frame_duration - elapsed);
  }

  // ── Cleanup ──────────────────────────────────────────────────────────
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();

  SDL_GL_MakeCurrent(window, nullptr);
  SDL_GL_DestroyContext(gl_ctx);
  SDL_DestroyWindow(window);

  std::cerr << "[XR_APILAYER_DEBUG_gui] GUI render thread stopped."
            << std::endl;
}

void gui_start(InstanceData *data) {
  if (data->gui_running.exchange(true))
    return; // already running

  std::cerr << "[XR_APILAYER_DEBUG_gui] Starting GUI thread." << std::endl;
  data->gui_thread = std::thread(gui_render_thread_func, data);
}

void gui_stop(InstanceData *data) {
  data->gui_running = false;
  if (data->gui_thread.joinable())
    data->gui_thread.join();
}

} // namespace debug_layer
