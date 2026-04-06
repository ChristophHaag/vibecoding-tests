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
// When `frames` and `frame_offset` are provided, the hover tooltip shows the frame number
// corresponding to the hovered data point.
static void DrawTimingGraph(const char *id, const char *label, const char *tooltip_text,
                            const std::vector<float> &buf, int count, float period_ms,
                            const std::vector<FramePerfRecord> *frames = nullptr,
                            int frame_offset = 0,
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

    // Remember position for custom hover tooltip
    ImVec2 plot_pos = ImGui::GetCursorScreenPos();
    ImVec2 plot_size = ImVec2(ImGui::GetContentRegionAvail().x, graph_height);

    ImGui::PlotLines(id, buf.data(), count, 0, overlay, 0.0f, y_max,
                     ImVec2(-1.0f, graph_height));

    // Custom hover: show frame number at the hovered sample
    if (frames && count > 1 &&
        ImGui::IsMouseHoveringRect(plot_pos, ImVec2(plot_pos.x + plot_size.x,
                                                     plot_pos.y + plot_size.y))) {
        float mouse_x = ImGui::GetMousePos().x;
        float t = (mouse_x - plot_pos.x) / plot_size.x;
        t = std::max(0.0f, std::min(1.0f, t));
        int idx = (int)(t * (float)(count - 1) + 0.5f);
        idx = std::max(0, std::min(count - 1, idx));

        int abs_idx = frame_offset + idx;
        if (abs_idx >= 0 && abs_idx < (int)frames->size()) {
            const auto &f = (*frames)[(size_t)abs_idx];
            ImGui::BeginTooltip();
            ImGui::Text("Frame #%llu  --  %.3f ms",
                        (unsigned long long)f.frame_number, buf[(size_t)idx]);
            if (idx > 0 && (abs_idx - 1) >= 0) {
                const auto &prev = (*frames)[(size_t)(abs_idx - 1)];
                ImGui::TextDisabled("transition #%llu -> #%llu",
                                    (unsigned long long)prev.frame_number,
                                    (unsigned long long)f.frame_number);
            }
            ImGui::EndTooltip();
        }
    }
}

// ── Bar segment colors ───────────────────────────────────────────────────────

static constexpr ImU32 COL_WAIT   = IM_COL32(60, 120, 220, 220);
static constexpr ImU32 COL_BEGIN  = IM_COL32(80, 200, 220, 220);
static constexpr ImU32 COL_APP    = IM_COL32(80, 200, 80, 220);
static constexpr ImU32 COL_SC     = IM_COL32(220, 160, 40, 220);
static constexpr ImU32 COL_END    = IM_COL32(220, 60, 60, 220);
static constexpr ImU32 COL_SYNC   = IM_COL32(140, 100, 220, 200);
static constexpr ImU32 COL_LOCV   = IM_COL32(180, 140, 220, 200);
static constexpr ImU32 COL_BUDGET = IM_COL32(255, 255, 80, 140);

// ── Stacked-bar drawing via ImDrawList ───────────────────────────────────────

struct BarSegment {
    float ms;
    ImU32 color;
    const char *label;
};

static void DrawStackedBar(const BarSegment *segs, int n, float total_ms, float avail_width,
                           float bar_height, uint64_t frame_number, float period_ms,
                           bool budget_mode, float scale_ref_ms)
{
    bool over = total_ms > period_ms && period_ms > 0.0f;
    float ratio = (period_ms > 0.0f) ? total_ms / period_ms : 0.0f;

    // Over-budget marker BEFORE the bar: "!" or ">>" for extreme outliers
    if (over && ratio > 2.0f) {
        char marker[16];
        snprintf(marker, sizeof(marker), "%.0fx", ratio);
        ImGui::TextColored(ImVec4(1.0f, 0.1f, 0.1f, 1.0f), "%s", marker);
        ImGui::SameLine(0.0f, 2.0f);
    } else if (over) {
        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "!");
        ImGui::SameLine(0.0f, 2.0f);
    } else {
        ImGui::TextDisabled(" ");
        ImGui::SameLine(0.0f, 2.0f);
    }

    // Frame number label
    char fn_label[24];
    snprintf(fn_label, sizeof(fn_label), "#%llu", (unsigned long long)frame_number);
    ImGui::TextDisabled("%s", fn_label);
    ImGui::SameLine(0.0f, 4.0f);

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // In budget mode: bar width = (total_ms / scale_ref_ms) * avail_width, clamped.
    // scale_ref_ms is either period_ms or max_frame_ms depending on the mode.
    // In percentage mode: bar width = avail_width.
    float bar_width;
    if (budget_mode && scale_ref_ms > 0.0f) {
        bar_width = (total_ms / scale_ref_ms) * avail_width;
        bar_width = std::max(bar_width, 4.0f);
        bar_width = std::min(bar_width, avail_width);
    } else {
        bar_width = avail_width;
    }

    // Background
    dl->AddRectFilled(p, ImVec2(p.x + bar_width, p.y + bar_height), IM_COL32(30, 30, 30, 200));

    if (total_ms > 0.0f) {
        float x = p.x;
        for (int i = 0; i < n; ++i) {
            float w = (segs[i].ms / total_ms) * bar_width;
            if (w < 1.0f && segs[i].ms > 0.0f)
                w = 1.0f;
            float x1 = std::min(x + w, p.x + bar_width);
            dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x1, p.y + bar_height), segs[i].color);
            x = x1;
        }
    }

    // Budget line in budget mode (marks 1x period)
    if (budget_mode && period_ms > 0.0f && scale_ref_ms > 0.0f) {
        float bx = p.x + (period_ms / scale_ref_ms) * avail_width;
        bx = std::min(bx, p.x + avail_width);
        dl->AddRectFilled(ImVec2(bx, p.y - 1), ImVec2(bx + 1, p.y + bar_height + 1),
                          COL_BUDGET);
    }

    // Hover tooltip
    if (ImGui::IsMouseHoveringRect(p, ImVec2(p.x + std::max(bar_width, avail_width),
                                              p.y + bar_height))) {
        ImGui::BeginTooltip();
        ImGui::Text("Frame #%llu  --  %.2f ms total", (unsigned long long)frame_number, total_ms);
        if (over)
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "  OVER BUDGET by %.2f ms",
                               total_ms - period_ms);
        for (int i = 0; i < n; ++i) {
            if (segs[i].ms > 0.001f)
                ImGui::Text("  %s: %.3f ms (%.0f%%)", segs[i].label, segs[i].ms,
                            segs[i].ms / total_ms * 100.0f);
        }
        ImGui::EndTooltip();
    }

    ImGui::Dummy(ImVec2(avail_width, bar_height));
}

// ── Real-time timeline ───────────────────────────────────────────────────────
// Each frame is a swim lane.  On a shared X axis (wall-clock time), colored
// rectangles show exactly when each phase started and ended.  This reveals
// pipeline overlap and idle gaps between frames.

static void DrawTimeline(const std::vector<FramePerfRecord> &frames, float period_ms,
                         int max_rows, float row_height, float zoom, float max_height,
                         float *zoom_inout)
{
    if (frames.empty())
        return;

    int total = (int)frames.size();
    int show = std::min(max_rows, total);
    int start_idx = total - show;

    // Time range
    int64_t t_min = frames[(size_t)start_idx].wait_frame_call_ts;
    int64_t t_max = frames.back().end_frame_return_ts;
    for (int i = start_idx; i < total; ++i) {
        if (frames[i].wait_frame_call_ts > 0 && frames[i].wait_frame_call_ts < t_min)
            t_min = frames[i].wait_frame_call_ts;
        if (frames[i].end_frame_return_ts > t_max)
            t_max = frames[i].end_frame_return_ts;
    }
    if (t_max <= t_min)
        return;

    // The visible area is the container width; the *content* is wider when zoomed.
    float container_w = ImGui::GetContentRegionAvail().x - 8;
    float content_w = container_w * zoom;
    float content_h = row_height * show + 4;
    float child_h = std::min(content_h, max_height);
    double ns_range = (double)(t_max - t_min);

    // Frame number label gutter width (reserve space left of the timeline)
    float gutter_w = (row_height >= 10.0f) ? ImGui::CalcTextSize("#99999").x + 4 : 0.0f;
    float timeline_w = content_w - gutter_w;
    if (timeline_w < 20.0f)
        timeline_w = 20.0f;

    auto ts_to_x = [&](int64_t ts, float origin_x) -> float {
        if (ts <= 0)
            return origin_x;
        return origin_x + (float)(((double)(ts - t_min) / ns_range) * (double)timeline_w);
    };

    ImGui::BeginChild("Timeline", ImVec2(-1, child_h), true,
                       ImGuiWindowFlags_HorizontalScrollbar);
    bool was_at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - row_height * 2;

    // Set a wider content size for horizontal scrolling
    if (zoom > 1.0f)
        ImGui::SetCursorPosX(0);  // reset to allow Dummy to define scroll extent

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // Shift timeline drawing right by gutter_w so labels fit left
    float tl_x0 = origin.x + gutter_w;

    // Background
    dl->AddRectFilled(ImVec2(tl_x0, origin.y),
                      ImVec2(tl_x0 + timeline_w, origin.y + row_height * show),
                      IM_COL32(20, 20, 25, 255));

    // VSync grid lines (use AddRectFilled for axis-aligned lines — much
    // faster than AddLine which goes through the expensive AddPolyline path).
    if (period_ms > 0.0f) {
        double period_ns = period_ms * 1e6;
        int64_t grid_start = t_min;
        for (int i = start_idx; i < total; ++i) {
            if (frames[i].predicted_display_time > 0) {
                grid_start = frames[i].predicted_display_time;
                while (grid_start > t_min)
                    grid_start -= (int64_t)period_ns;
                break;
            }
        }
        float grid_bottom = origin.y + row_height * show;
        int grid_count = 0;
        for (int64_t gt = grid_start; gt <= t_max && grid_count < 500; gt += (int64_t)period_ns, ++grid_count) {
            float gx = ts_to_x(gt, tl_x0);
            dl->AddRectFilled(ImVec2(gx, origin.y), ImVec2(gx + 1, grid_bottom),
                              IM_COL32(60, 60, 40, 100));
        }
    }

    // Draw each frame's phases
    for (int ri = 0; ri < show; ++ri) {
        const auto &f = frames[(size_t)(start_idx + ri)];
        float y0 = origin.y + row_height * ri + 1;
        float y1 = y0 + row_height - 2;

        // Frame number label in gutter (if rows are tall enough)
        if (gutter_w > 0.0f) {
            char fn_buf[16];
            snprintf(fn_buf, sizeof(fn_buf), "#%llu", (unsigned long long)f.frame_number);
            dl->AddText(ImVec2(origin.x + 2, y0), IM_COL32(180, 180, 180, 180), fn_buf);
        }

        auto draw_phase = [&](int64_t ts0, int64_t ts1, ImU32 col) {
            if (ts0 <= 0 || ts1 <= 0 || ts1 <= ts0)
                return;
            float x0f = ts_to_x(ts0, tl_x0);
            float x1f = ts_to_x(ts1, tl_x0);
            if (x1f - x0f < 1.0f)
                x1f = x0f + 1.0f;
            dl->AddRectFilled(ImVec2(x0f, y0), ImVec2(x1f, y1), col);
        };

        draw_phase(f.wait_frame_call_ts, f.wait_frame_return_ts, COL_WAIT);
        draw_phase(f.begin_frame_call_ts, f.begin_frame_return_ts, COL_BEGIN);
        draw_phase(f.begin_frame_return_ts, f.end_frame_call_ts, COL_APP);
        draw_phase(f.sync_actions_call_ts, f.sync_actions_return_ts, COL_SYNC);
        draw_phase(f.locate_views_call_ts, f.locate_views_return_ts, COL_LOCV);
        for (auto &sc : f.swapchains) {
            draw_phase(sc.acquire_call_ts, sc.acquire_done_ts, COL_SC);
            draw_phase(sc.wait_call_ts, sc.wait_done_ts, COL_SC);
            draw_phase(sc.release_ts, sc.release_done_ts, COL_SC);
        }
        draw_phase(f.end_frame_call_ts, f.end_frame_return_ts, COL_END);

        // Predicted display time marker (use rect instead of line for perf)
        if (f.predicted_display_time > 0) {
            float dx = ts_to_x(f.predicted_display_time, tl_x0);
            dl->AddRectFilled(ImVec2(dx, y0), ImVec2(dx + 2, y1), COL_BUDGET);
        }
    }

    // Hover tooltip
    ImVec2 mouse = ImGui::GetMousePos();
    if (ImGui::IsMouseHoveringRect(ImVec2(tl_x0, origin.y),
                                   ImVec2(tl_x0 + timeline_w,
                                          origin.y + row_height * show))) {
        int row = (int)((mouse.y - origin.y) / row_height);
        if (row >= 0 && row < show) {
            const auto &f = frames[(size_t)(start_idx + row)];
            ImGui::BeginTooltip();
            ImGui::Text("Frame #%llu  --  %.2f ms", (unsigned long long)f.frame_number,
                        f.total_frame_ms);
            ImGui::Text("  Wait: %.2f  Begin: %.2f  App: %.2f  End: %.2f",
                        f.wait_frame_ms, f.begin_frame_ms, f.app_work_ms, f.end_frame_ms);
            ImGui::Text("  SC: %.2f  Sync: %.2f  Views: %.2f",
                        f.swapchain_acquire_wait_ms, f.sync_actions_ms, f.locate_views_ms);
            float wall_ms = (float)((f.end_frame_return_ts - f.wait_frame_call_ts) * 1e-6);
            ImGui::TextDisabled("  wall span: %.2f ms", wall_ms);
            ImGui::EndTooltip();
        }
    }

    ImGui::Dummy(ImVec2(content_w, row_height * show));

    // Auto-scroll to latest frames (bottom) unless user has scrolled up
    if (was_at_bottom)
        ImGui::SetScrollHereY(1.0f);

    // Ctrl + mouse wheel to zoom the time axis
    if (zoom_inout && ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
            *zoom_inout = std::clamp(*zoom_inout * (1.0f + wheel * 0.15f), 1.0f, 100.0f);
    }

    ImGui::EndChild();
}

// ── Main panel ───────────────────────────────────────────────────────────────

void gui_render_perf_panel(InstanceData *data)
{
    // Per-panel persistent state
    static bool paused = false;
    static std::vector<FramePerfRecord> snapshot; // safe local copy
    static float graph_seconds = 5.0f;
    static std::vector<float> buf;
    static bool budget_mode = true; // true = real-time budget, false = percentage
    static int scale_mode = 0;     // 0 = period, 1 = max frame in window
    static float timeline_zoom = 1.0f;
    static float timeline_row_h = 16.0f;
    static float timeline_max_h = 250.0f;

    bool panel_visible = ImGui::Begin("Performance");
    if (!panel_visible) {
        ImGui::End();
        return;
    }

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

    auto ms_color = [&](float ms) -> ImVec4 {
        if (ms <= period_ms * 0.8f)
            return ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
        if (ms <= period_ms)
            return ImVec4(1.0f, 1.0f, 0.3f, 1.0f);
        return ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
    };

    ImGui::Text("Frame #%llu", (unsigned long long)latest.frame_number);
    ImGui::SameLine();
    ImGui::TextColored(ms_color(latest.total_frame_ms), "  Total: %.2f ms", latest.total_frame_ms);
    ImGui::SameLine();
    ImGui::Text(" |  Budget: %.2f ms (%.0f Hz)", period_ms, 1000.0f / period_ms);

    // CPU headroom: how much of the frame budget is consumed by actual work
    // (everything except xrWaitFrame, which is the runtime's pacing sleep).
    float work_ms = latest.total_frame_ms - latest.wait_frame_ms;
    float headroom_pct = (period_ms > 0.0f) ? (1.0f - work_ms / period_ms) * 100.0f : 0.0f;
    ImGui::SameLine();
    ImGui::TextColored(ms_color(work_ms),
                       "  Headroom: %.0f%% (%.2f ms work)", headroom_pct, work_ms);

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
    if (ImGui::CollapsingHeader("Frame Breakdown", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::RadioButton("Budget scale", budget_mode)) budget_mode = true;
    ImGui::SameLine();
    if (ImGui::RadioButton("Percentage", !budget_mode)) budget_mode = false;
    ImGui::SameLine();
    HelpTooltip(
        "Budget scale: bar width proportional to frame time vs a reference.\n"
        "  The yellow line marks the display period (1x budget).\n"
        "  'Period' reference: 1x budget = full width. Over-budget bars extend past.\n"
        "  'Max frame' reference: the longest frame in the window fills the width.\n"
        "    Better for seeing relative differences when frames vary wildly.\n\n"
        "Percentage: every bar fills the full width.\n"
        "  Useful for seeing proportions even for fast frames.");

    // Scale reference selector (only when budget mode is on)
    if (budget_mode) {
        ImGui::SameLine(0.0f, 20.0f);
        ImGui::TextDisabled("Ref:");
        ImGui::SameLine();
        if (ImGui::RadioButton("Period##ref", scale_mode == 0)) scale_mode = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Max frame##ref", scale_mode == 1)) scale_mode = 1;
    }

    ImGui::SameLine(0.0f, 20.0f);
    ImGui::Text("Breakdown  (#%llu .. #%llu)",
                (unsigned long long)frames.front().frame_number,
                (unsigned long long)frames.back().frame_number);

    float bar_region_w = ImGui::GetContentRegionAvail().x - 80; // room for marker + frame# label
    // Limit visible bars for performance
    int visible_bars = std::min(max_show, 150);
    int bar_vis_start = max_show - visible_bars;

    // Compute max frame time in visible bars for "Max frame" scale mode.
    float max_frame_ms = 0.0f;
    for (int i = bar_vis_start; i < bar_vis_start + visible_bars; ++i)
        max_frame_ms = std::max(max_frame_ms, frames[(size_t)i].total_frame_ms);
    float scale_ref_ms;
    if (!budget_mode)
        scale_ref_ms = period_ms; // unused in percentage mode, but safe
    else if (scale_mode == 1 && max_frame_ms > 0.0f)
        scale_ref_ms = max_frame_ms * 1.05f; // 5% padding so longest bar doesn't quite touch edge
    else
        scale_ref_ms = period_ms;

    float bar_h = std::max(3.0f, std::min(8.0f, 400.0f / (float)visible_bars));

    ImGui::BeginChild("BarChart", ImVec2(-1, bar_h * visible_bars + 4), true);
    for (int i = 0; i < visible_bars; ++i) {
        const auto &f = frames[(size_t)(bar_vis_start + i)];
        BarSegment segs[] = {
            {f.wait_frame_ms,             COL_WAIT,  "xrWaitFrame"},
            {f.begin_frame_ms,            COL_BEGIN, "xrBeginFrame"},
            {f.app_work_ms,               COL_APP,   "App work"},
            {f.swapchain_acquire_wait_ms, COL_SC,    "Swapchain acquire+wait"},
            {f.end_frame_ms,              COL_END,   "xrEndFrame"},
        };
        DrawStackedBar(segs, 5, f.total_frame_ms, bar_region_w, bar_h,
                       f.frame_number, period_ms, budget_mode, scale_ref_ms);
    }
    ImGui::EndChild();
    } // Frame Breakdown

    ImGui::Separator();

    // ── Real-time timeline ───────────────────────────────────────────
    if (ImGui::CollapsingHeader("Real-Time Timeline", ImGuiTreeNodeFlags_DefaultOpen)) {
    HelpTooltip(
        "Each row is one frame. The horizontal axis is wall-clock time.\n"
        "Colored rectangles show exactly when each API call started and ended.\n\n"
        "This reveals:\n"
        "  - Pipeline overlap: frame N+1's xrWaitFrame returning while frame N\n"
        "    is still being composed in xrEndFrame.\n"
        "  - Idle gaps: dark space between phases means the app or runtime is idle.\n"
        "  - VSync alignment: yellow vertical lines = predicted display times.\n\n"
        "Colors match the bar chart. Purple=SyncActions, Light purple=LocateViews.\n"
        "Ctrl+Scroll to zoom. Scroll up/down to navigate frames.");
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("Zoom##tl", &timeline_zoom, 1.0f, 100.0f, "%.1fx");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::SliderFloat("Row H##tl", &timeline_row_h, 8.0f, 32.0f, "%.0f px");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::SliderFloat("Height##tl", &timeline_max_h, 100.0f, 600.0f, "%.0f px");

    DrawTimeline(frames, period_ms, max_show, timeline_row_h, timeline_zoom,
                 timeline_max_h, &timeline_zoom);
    } // Real-Time Timeline

    ImGui::Separator();

    // ── Individual timing graphs ─────────────────────────────────────
    if (ImGui::CollapsingHeader("Timing Graphs", ImGuiTreeNodeFlags_DefaultOpen)) {
    // All share the same X extent (max_show frames) so they're visually aligned.
    // frame_offset: index into `frames` where the shown data starts
    int frame_offset = (int)frames.size() - max_show;
    if (frame_offset < 0) frame_offset = 0;

    int n;
    float graph_h = 55.0f;

    n = extract_recent(frames, max_show, buf,
                       [](const FramePerfRecord &f) { return f.total_frame_ms; });
    DrawTimingGraph("##total", "Total Frame Time",
                    "Wall-clock time from when the app calls xrWaitFrame to when "
                    "xrEndFrame returns. Should stay <= the display period for smooth "
                    "rendering. Spikes here mean a dropped frame.",
                    buf, n, period_ms, &frames, frame_offset, graph_h);

    n = extract_recent(frames, max_show, buf,
                       [](const FramePerfRecord &f) { return f.wait_frame_ms; });
    DrawTimingGraph("##wait", "xrWaitFrame Duration",
                    "How long the runtime blocked the app in xrWaitFrame. The runtime "
                    "uses this to pace the application to the display refresh rate. "
                    "A short wait means the app barely finished in time. "
                    "If this drops to near zero, the app is probably missing frames.",
                    buf, n, period_ms, &frames, frame_offset, graph_h);

    n = extract_recent(frames, max_show, buf,
                       [](const FramePerfRecord &f) { return f.app_work_ms; });
    DrawTimingGraph("##appwork", "App CPU Work",
                    "Time between xrBeginFrame returning and xrEndFrame being called. "
                    "This is the app's own CPU time: scene traversal, render command "
                    "recording, xrSyncActions, xrLocateViews, xrLocateSpace calls, etc. "
                    "If this approaches the display period, the app is CPU-bound.",
                    buf, n, period_ms, &frames, frame_offset, graph_h);

    n = extract_recent(frames, max_show, buf,
                       [](const FramePerfRecord &f) { return f.end_frame_ms; });
    DrawTimingGraph("##endframe", "xrEndFrame Duration",
                    "How long xrEndFrame took to return. The spec says this should be "
                    "fast, but some runtimes block here for composition or GPU sync. "
                    "Large values indicate runtime-side bottlenecks.",
                    buf, n, period_ms, &frames, frame_offset, graph_h);

    n = extract_recent(frames, max_show, buf,
                       [](const FramePerfRecord &f) { return f.swapchain_acquire_wait_ms; });
    DrawTimingGraph("##scwait", "Swapchain Acquire + Wait",
                    "Combined time the app spent in xrAcquireSwapchainImage + "
                    "xrWaitSwapchainImage across all swapchains this frame. "
                    "Large values mean the GPU hasn't finished rendering the previous "
                    "frame's swapchain images: the app is GPU-bound.",
                    buf, n, period_ms, &frames, frame_offset, graph_h);

    n = extract_recent(frames, max_show, buf,
                       [](const FramePerfRecord &f) { return f.begin_frame_ms; });
    DrawTimingGraph("##beginframe", "xrBeginFrame Duration",
                    "xrBeginFrame marks the start of the submission window. "
                    "Should be near-instant. If it blocks, the runtime may be "
                    "throttling frame submission.",
                    buf, n, period_ms, &frames, frame_offset, graph_h);

    // Smaller secondary graphs
    float small_h = 40.0f;

    n = extract_recent(frames, max_show, buf,
                       [](const FramePerfRecord &f) { return f.sync_actions_ms; });
    DrawTimingGraph("##sync", "xrSyncActions",
                    "Time spent in xrSyncActions (polls controller input from the "
                    "runtime). Usually sub-millisecond, but can spike if the runtime's "
                    "input subsystem is busy.",
                    buf, n, period_ms, &frames, frame_offset, small_h);

    n = extract_recent(frames, max_show, buf,
                       [](const FramePerfRecord &f) { return f.locate_views_ms; });
    DrawTimingGraph("##locviews", "xrLocateViews",
                    "Time spent querying head pose and FOV for rendering. Usually "
                    "trivial, but worth monitoring for prediction pipeline issues.",
                    buf, n, period_ms, &frames, frame_offset, small_h);
    } // Timing Graphs

    // ── Legend ────────────────────────────────────────────────────────
    ImGui::Separator();
    ImGui::TextDisabled("Colors:");
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
    ImGui::TextColored(ImVec4(0.55f, 0.39f, 0.86f, 1.0f), "Sync");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.71f, 0.55f, 0.86f, 1.0f), "Views");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.31f, 1.0f), "|=VSync");
    ImGui::SameLine();
    ImGui::TextDisabled("  ! = over budget");

    ImGui::End();
}

} // namespace debug_layer
