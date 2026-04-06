// SPDX-License-Identifier: MIT
// gui/gui_perf.cpp — Performance observatory: frame lifecycle timing visualisation.
//
// Design rationale
// ────────────────
// The panel is designed so an app developer can see at a glance:
//   1. Where time goes each frame (stacked bar chart — runtime wait vs. app work vs. submit)
//   2. Whether any phase is spiking (individual line graphs show history)
//   3. How phases relate to each other over time (shared X-axis, aligned)
//   4. Anomalies in swapchain acquire/wait (GPU back-pressure) and xrEndFrame blocking
//
// Every graph has:
//   • A clear title explaining *what* it measures
//   • A (?) tooltip icon with a longer plain-English explanation
//   • Units on the overlay text

#include "gui_perf.h"
#include "../instance_data.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <shared_mutex>
#include <vector>

namespace debug_layer {

// ── Helpers ──────────────────────────────────────────────────────────────────

// Draw a (?) marker; if the user hovers it, show a tooltip with `text`.
static void HelpTooltip(const char *text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(400.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Extract the N most-recent values of `field` from `frames` into `buf`.
// Returns the actual count written.
template <typename Fn>
static int extract_recent(const std::vector<FramePerfRecord> &frames, int max_show,
                          std::vector<float> &buf, Fn getter)
{
    int total = (int)frames.size();
    int show = std::min(max_show, total);
    if (show <= 0)
        return 0;
    buf.resize((size_t)show);
    int start = total - show;
    for (int i = 0; i < show; ++i)
        buf[i] = getter(frames[(size_t)(start + i)]);
    return show;
}

// Draw a labelled line graph with auto-scaled Y, a period reference line, and overlay text.
static void DrawTimingGraph(const char *id, const char *label, const char *tooltip_text,
                            const std::vector<float> &buf, int count, float period_ms,
                            float graph_height = 60.0f)
{
    if (count <= 0) {
        ImGui::TextDisabled("%s: (no data)", label);
        return;
    }

    ImGui::Text("%s", label);
    HelpTooltip(tooltip_text);

    // Compute stats for overlay
    float latest = buf[(size_t)(count - 1)];
    float mn = *std::min_element(buf.begin(), buf.begin() + count);
    float mx = *std::max_element(buf.begin(), buf.begin() + count);
    float avg = std::accumulate(buf.begin(), buf.begin() + count, 0.0f) / (float)count;

    char overlay[128];
    snprintf(overlay, sizeof(overlay), "%.2f ms  avg %.2f  min %.2f  max %.2f",
             latest, avg, mn, mx);

    // Y range: 0 to max(2*period, 1.2*max_value) to keep period line visible
    float y_max = std::max(period_ms * 2.0f, mx * 1.2f);
    y_max = std::max(y_max, 0.5f); // never zero

    ImGui::PlotLines(id, buf.data(), count, 0, overlay, 0.0f, y_max,
                     ImVec2(-1.0f, graph_height));
}

// ── Stacked-bar drawing via ImDrawList ───────────────────────────────────────

struct BarSegment {
    float ms;
    ImU32 color;
    const char *label;
};

static void DrawStackedBar(const BarSegment *segs, int n, float total_ms, float bar_width,
                           float bar_height, uint64_t frame_number, float period_ms)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // Background
    dl->AddRectFilled(p, ImVec2(p.x + bar_width, p.y + bar_height), IM_COL32(30, 30, 30, 200));

    if (total_ms <= 0.0f) {
        ImGui::Dummy(ImVec2(bar_width, bar_height));
        return;
    }

    // Red tint on the background if frame exceeded budget
    if (total_ms > period_ms && period_ms > 0.0f) {
        dl->AddRectFilled(p, ImVec2(p.x + bar_width, p.y + bar_height),
                          IM_COL32(180, 30, 30, 40));
    }

    float x = p.x;
    for (int i = 0; i < n; ++i) {
        float w = (segs[i].ms / total_ms) * bar_width;
        if (w < 1.0f)
            w = 1.0f;
        float x1 = std::min(x + w, p.x + bar_width);
        dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x1, p.y + bar_height), segs[i].color);
        x = x1;
    }

    // Whole-bar hover tooltip with frame number + breakdown
    if (ImGui::IsMouseHoveringRect(p, ImVec2(p.x + bar_width, p.y + bar_height))) {
        ImGui::BeginTooltip();
        ImGui::Text("Frame #%llu  —  %.2f ms total", (unsigned long long)frame_number, total_ms);
        if (total_ms > period_ms && period_ms > 0.0f)
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "  OVER BUDGET by %.2f ms",
                               total_ms - period_ms);
        for (int i = 0; i < n; ++i) {
            if (segs[i].ms > 0.001f)
                ImGui::Text("  %s: %.3f ms (%.0f%%)", segs[i].label, segs[i].ms,
                            segs[i].ms / total_ms * 100.0f);
        }
        ImGui::EndTooltip();
    }

    ImGui::Dummy(ImVec2(bar_width, bar_height));
}

// ── Main panel ───────────────────────────────────────────────────────────────

void gui_render_perf_panel(InstanceData *data)
{
    // Per-panel persistent state
    static bool paused = false;
    static std::vector<FramePerfRecord> snapshot; // safe local copy
    static float graph_seconds = 5.0f;
    static std::vector<float> buf;

    ImGui::Begin("Performance");

    // ── Pause / Resume ───────────────────────────────────────────────
    if (paused) {
        if (ImGui::Button("Resume")) {
            paused = false;
            snapshot.clear();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "PAUSED (%zu frames frozen)",
                           snapshot.size());
    } else {
        if (ImGui::Button("Pause")) {
            paused = true;
            // Snapshot is taken below under the lock
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Live");
    }

    ImGui::SameLine(0.0f, 20.0f);
    ImGui::SetNextItemWidth(150.0f);
    ImGui::SliderFloat("History##perf", &graph_seconds, 1.0f, 30.0f, "%.0f s");
    HelpTooltip("How many seconds of frame history to show in graphs.");

    ImGui::Separator();

    // ── Copy frames under lock into a local vector ───────────────────
    // CRITICAL: we must not hold a pointer/reference to data->perf.frames
    // outside the lock — the interceptor thread mutates the deque and can
    // invalidate iterators/pointers at any time.
    float period_ms;
    if (!paused) {
        std::shared_lock lock(data->state_mutex);
        period_ms = (data->frame_state.predicted_display_period > 0)
                        ? (float)(data->frame_state.predicted_display_period * 1e-6)
                        : 11.11f;
        int max_needed = std::max(1, (int)(graph_seconds * 1000.0f / period_ms));
        max_needed = std::min(max_needed, (int)data->perf.frames.size());
        int start = (int)data->perf.frames.size() - max_needed;
        snapshot.assign(data->perf.frames.begin() + start, data->perf.frames.end());
    } else {
        std::shared_lock lock(data->state_mutex);
        period_ms = (data->frame_state.predicted_display_period > 0)
                        ? (float)(data->frame_state.predicted_display_period * 1e-6)
                        : 11.11f;
    }

    const auto &frames = snapshot;

    if (frames.empty()) {
        ImGui::TextDisabled("Waiting for frames...");
        ImGui::End();
        return;
    }

    int max_show = (int)frames.size();

    // ── Latest frame summary ─────────────────────────────────────────
    const auto &latest = frames.back();
    ImGui::Text("Frame #%llu", (unsigned long long)latest.frame_number);

    // Color-code: green if under budget, yellow if close, red if over
    auto ms_color = [&](float ms) -> ImVec4 {
        if (ms <= period_ms * 0.8f)
            return ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
        if (ms <= period_ms)
            return ImVec4(1.0f, 1.0f, 0.3f, 1.0f);
        return ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
    };

    ImGui::TextColored(ms_color(latest.total_frame_ms), "Total: %.2f ms", latest.total_frame_ms);
    ImGui::SameLine();
    ImGui::Text(" |  Budget: %.2f ms (%.0f Hz)", period_ms, 1000.0f / period_ms);

    // Compact summary line
    ImGui::Text("  Wait: %.2f  Begin: %.2f  App: %.2f  End: %.2f  SC: %.2f  Sync: %.2f  Views: %.2f",
                latest.wait_frame_ms, latest.begin_frame_ms, latest.app_work_ms,
                latest.end_frame_ms, latest.swapchain_acquire_wait_ms,
                latest.sync_actions_ms, latest.locate_views_ms);

    // Dropped-frame counter (frames where total > period)
    if (period_ms > 0.0f) {
        int dropped = 0;
        for (auto &f : frames) {
            if (f.total_frame_ms > period_ms)
                dropped++;
        }
        if (dropped > 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                               "  [%d over-budget in window]", dropped);
        }
    }

    ImGui::Separator();

    // ── Stacked-bar chart for last N frames ──────────────────────────
    ImGui::Text("Frame Budget Breakdown  (frames #%llu .. #%llu)",
                (unsigned long long)frames.front().frame_number,
                (unsigned long long)frames.back().frame_number);
    HelpTooltip(
        "Each horizontal bar shows how one frame's time is divided:\n"
        "  Blue = xrWaitFrame (runtime pacing, app sleeps here)\n"
        "  Cyan = xrBeginFrame (marks start of GPU work submission window)\n"
        "  Green = App work (CPU rendering between BeginFrame->EndFrame)\n"
        "  Orange = Swapchain acquire+wait (GPU back-pressure)\n"
        "  Red = xrEndFrame (composition submit, may block on some runtimes)\n\n"
        "Bars with a red tint exceeded the frame budget.\n"
        "Hover any bar for a per-frame breakdown with frame number.\n\n"
        "If 'App work' (green) approaches the budget line, the app is CPU-bound.\n"
        "If 'SC acquire/wait' (orange) is large, the app is GPU-bound.\n"
        "If 'xrEndFrame' (red) is large, the runtime compositor is slow.");

    float bar_width = ImGui::GetContentRegionAvail().x;
    // Limit visible bars for performance
    int visible_bars = std::min(max_show, 150);
    int bar_vis_start = max_show - visible_bars;

    float bar_h = std::max(3.0f, std::min(8.0f, 400.0f / (float)visible_bars));

    ImGui::BeginChild("BarChart", ImVec2(-1, bar_h * visible_bars + 4), true);
    for (int i = 0; i < visible_bars; ++i) {
        const auto &f = frames[(size_t)(bar_vis_start + i)];
        BarSegment segs[] = {
            {f.wait_frame_ms,             IM_COL32(60, 120, 220, 220), "xrWaitFrame"},
            {f.begin_frame_ms,            IM_COL32(80, 200, 220, 220), "xrBeginFrame"},
            {f.app_work_ms,               IM_COL32(80, 200, 80, 220),  "App work"},
            {f.swapchain_acquire_wait_ms, IM_COL32(220, 160, 40, 220), "Swapchain acquire+wait"},
            {f.end_frame_ms,              IM_COL32(220, 60, 60, 220),  "xrEndFrame"},
        };
        DrawStackedBar(segs, 5, f.total_frame_ms, bar_width - 16, bar_h,
                       f.frame_number, period_ms);
    }
    ImGui::EndChild();

    ImGui::Separator();

    // ── Individual timing graphs ─────────────────────────────────────
    // All share the same X extent (max_show frames) so they're visually aligned.

    int n;
    float graph_h = 55.0f;

    n = extract_recent(frames, max_show, buf,
                       [](const FramePerfRecord &f) { return f.total_frame_ms; });
    DrawTimingGraph("##total", "Total Frame Time",
                    "Wall-clock time from when the app calls xrWaitFrame to when "
                    "xrEndFrame returns. Should stay <= the display period for smooth "
                    "rendering. Spikes here mean a dropped frame.",
                    buf, n, period_ms, graph_h);

    n = extract_recent(frames, max_show, buf,
                       [](const FramePerfRecord &f) { return f.wait_frame_ms; });
    DrawTimingGraph("##wait", "xrWaitFrame Duration",
                    "How long the runtime blocked the app in xrWaitFrame. The runtime "
                    "uses this to pace the application to the display refresh rate. "
                    "A short wait means the app barely finished in time. "
                    "If this drops to near zero, the app is probably missing frames.",
                    buf, n, period_ms, graph_h);

    n = extract_recent(frames, max_show, buf,
                       [](const FramePerfRecord &f) { return f.app_work_ms; });
    DrawTimingGraph("##appwork", "App CPU Work",
                    "Time between xrBeginFrame returning and xrEndFrame being called. "
                    "This is the app's own CPU time: scene traversal, render command "
                    "recording, xrSyncActions, xrLocateViews, xrLocateSpace calls, etc. "
                    "If this approaches the display period, the app is CPU-bound.",
                    buf, n, period_ms, graph_h);

    n = extract_recent(frames, max_show, buf,
                       [](const FramePerfRecord &f) { return f.end_frame_ms; });
    DrawTimingGraph("##endframe", "xrEndFrame Duration",
                    "How long xrEndFrame took to return. The spec says this should be "
                    "fast, but some runtimes block here for composition or GPU sync. "
                    "Large values indicate runtime-side bottlenecks.",
                    buf, n, period_ms, graph_h);

    n = extract_recent(frames, max_show, buf,
                       [](const FramePerfRecord &f) { return f.swapchain_acquire_wait_ms; });
    DrawTimingGraph("##scwait", "Swapchain Acquire + Wait",
                    "Combined time the app spent in xrAcquireSwapchainImage + "
                    "xrWaitSwapchainImage across all swapchains this frame. "
                    "Large values mean the GPU hasn't finished rendering the previous "
                    "frame's swapchain images: the app is GPU-bound.",
                    buf, n, period_ms, graph_h);

    n = extract_recent(frames, max_show, buf,
                       [](const FramePerfRecord &f) { return f.begin_frame_ms; });
    DrawTimingGraph("##beginframe", "xrBeginFrame Duration",
                    "xrBeginFrame marks the start of the submission window. "
                    "Should be near-instant. If it blocks, the runtime may be "
                    "throttling frame submission.",
                    buf, n, period_ms, graph_h);

    // Smaller secondary graphs
    float small_h = 40.0f;

    n = extract_recent(frames, max_show, buf,
                       [](const FramePerfRecord &f) { return f.sync_actions_ms; });
    DrawTimingGraph("##sync", "xrSyncActions",
                    "Time spent in xrSyncActions (polls controller input from the "
                    "runtime). Usually sub-millisecond, but can spike if the runtime's "
                    "input subsystem is busy.",
                    buf, n, period_ms, small_h);

    n = extract_recent(frames, max_show, buf,
                       [](const FramePerfRecord &f) { return f.locate_views_ms; });
    DrawTimingGraph("##locviews", "xrLocateViews",
                    "Time spent querying head pose and FOV for rendering. Usually "
                    "trivial, but worth monitoring for prediction pipeline issues.",
                    buf, n, period_ms, small_h);

    // ── Legend ────────────────────────────────────────────────────────
    ImGui::Separator();
    ImGui::TextDisabled("Bar colors:");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.24f, 0.47f, 0.86f, 1.0f), "Wait");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.31f, 0.78f, 0.86f, 1.0f), "Begin");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.31f, 0.78f, 0.31f, 1.0f), "App");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.86f, 0.63f, 0.16f, 1.0f), "SC");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.86f, 0.24f, 0.24f, 1.0f), "End");
    ImGui::SameLine();
    ImGui::TextDisabled("  |  Red tint = over budget");

    ImGui::End();
}

} // namespace debug_layer
