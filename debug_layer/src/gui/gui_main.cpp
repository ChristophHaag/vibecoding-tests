// SPDX-License-Identifier: MIT
// gui/gui_main.cpp — SDL3 + Dear ImGui debug window on a dedicated thread.

#include "gui_main.h"
#include "gui_actions.h"
#include "gui_spaces.h"
#include "../instance_data.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace debug_layer {

static void gui_render_thread_func(InstanceData *data)
{
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
        std::cerr << "[XR_APILAYER_DEBUG_gui] SDL_Init(VIDEO) failed: " << SDL_GetError()
                  << std::endl;
        data->gui_running = false;
        return;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);

    SDL_Window *window = SDL_CreateWindow("OpenXR Debug GUI", 1280, 800,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        std::cerr << "[XR_APILAYER_DEBUG_gui] SDL_CreateWindow failed: " << SDL_GetError()
                  << std::endl;
        data->gui_running = false;
        return;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (gl_ctx == nullptr) {
        std::cerr << "[XR_APILAYER_DEBUG_gui] SDL_GL_CreateContext failed: " << SDL_GetError()
                  << std::endl;
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

    // ── Set up first-run default layout ──────────────────────────────────
    // Only runs when imgui.ini doesn't exist yet.
    bool first_run = !SDL_GetPathInfo(ini_path.c_str(), nullptr);
    bool want_initial_layout = first_run;

    std::cerr << "[XR_APILAYER_DEBUG_gui] GUI render thread started." << std::endl;

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
        ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                                            ImGuiDockNodeFlags_None);

        // ── First-run layout ─────────────────────────────────────────
        if (want_initial_layout) {
            want_initial_layout = false;

            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            int win_w, win_h;
            SDL_GetWindowSizeInPixels(window, &win_w, &win_h);
            ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2((float)win_w, (float)win_h));

            // Split: left 55% for 3D view, right 45% for panels
            ImGuiID left_id, right_id;
            ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.55f, &left_id, &right_id);

            // Split right into top (live state + frame info) and bottom (actions + bindings)
            ImGuiID right_top_id, right_bottom_id;
            ImGui::DockBuilderSplitNode(right_id, ImGuiDir_Up, 0.55f, &right_top_id, &right_bottom_id);

            ImGui::DockBuilderDockWindow("3D Spaces", left_id);
            ImGui::DockBuilderDockWindow("Active Profiles & Live State", right_top_id);
            ImGui::DockBuilderDockWindow("Frame Info", right_top_id);
            ImGui::DockBuilderDockWindow("Action Sets & Actions", right_bottom_id);
            ImGui::DockBuilderDockWindow("Suggested Bindings", right_bottom_id);

            ImGui::DockBuilderFinish(dockspace_id);
        }

        // ── Render panels ────────────────────────────────────────────
        gui_render_actions_panel(data);
        gui_render_spaces_panel(data);

        // ── Render frame info overlay ────────────────────────────────
        {
            std::shared_lock lock(data->state_mutex);
            ImGui::Begin("Frame Info");
            ImGui::Text("Frame #%llu", (unsigned long long)data->frame_state.frame_count);
            ImGui::Text("Predicted display time: %lld ns",
                        (long long)data->frame_state.predicted_display_time);
            if (data->frame_state.predicted_display_period > 0) {
                double fps = 1e9 / (double)data->frame_state.predicted_display_period;
                ImGui::Text("Target refresh: %.1f Hz", fps);
            }
            ImGui::Text("Should render: %s",
                        data->frame_state.should_render ? "true" : "false");

            // Session states
            for (auto &[handle, s] : data->sessions) {
                ImGui::Text("Session %p: state=%d", (void *)handle, (int)s.state);
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

    std::cerr << "[XR_APILAYER_DEBUG_gui] GUI render thread stopped." << std::endl;
}

void gui_start(InstanceData *data)
{
    if (data->gui_running.exchange(true))
        return; // already running

    std::cerr << "[XR_APILAYER_DEBUG_gui] Starting GUI thread." << std::endl;
    data->gui_thread = std::thread(gui_render_thread_func, data);
}

void gui_stop(InstanceData *data)
{
    data->gui_running = false;
    if (data->gui_thread.joinable())
        data->gui_thread.join();
}

} // namespace debug_layer
