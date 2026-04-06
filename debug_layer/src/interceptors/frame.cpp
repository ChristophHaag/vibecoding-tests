// SPDX-License-Identifier: MIT
// interceptors/frame.cpp — xrWaitFrame, xrBeginFrame, xrEndFrame, swapchain ops.

#include "../dispatch.h"
#include "../instance_data.h"

#include <ctime>

namespace debug_layer {

// CLOCK_MONOTONIC nanoseconds — same timebase as XrTime on Linux/Monado.
static int64_t now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static float ns_to_ms(int64_t a, int64_t b)
{
    return (float)((b - a) * 1e-6);
}

// ── xrWaitFrame ──────────────────────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrWaitFrame(XrSession session, const XrFrameWaitInfo *frameWaitInfo,
                                       XrFrameState *frameState)
{
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
        data->frame_state.predicted_display_period = frameState->predictedDisplayPeriod;
        data->frame_state.should_render = frameState->shouldRender;
        data->frame_state.frame_count++;

        XrTime new_time = frameState->predictedDisplayTime;
        if (data->frame_state.prev_display_time != 0 && new_time > 0) {
            float delta_ms = (float)((new_time - data->frame_state.prev_display_time) * 1e-6);
            auto &fs = data->frame_state;
            fs.timing_deltas_ms[fs.timing_write_idx] = delta_ms;
            fs.timing_write_idx = (fs.timing_write_idx + 1) % TrackedFrameState::kMaxTimingSamples;
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

XrResult XRAPI_CALL Layer_xrBeginFrame(XrSession session, const XrFrameBeginInfo *frameBeginInfo)
{
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

XrResult XRAPI_CALL Layer_xrEndFrame(XrSession session, const XrFrameEndInfo *frameEndInfo)
{
    InstanceData *data = GetInstanceDataFromSession(session);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    int64_t t0 = now_ns();
    XrResult result = data->next.xrEndFrame(session, frameEndInfo);
    int64_t t1 = now_ns();

    {
        std::unique_lock lock(data->state_mutex);
        auto &perf = data->perf;
        if (perf.has_current) {
            auto &c = perf.current;
            c.end_frame_call_ts = t0;
            c.end_frame_return_ts = t1;

            // Compute derived durations
            c.wait_frame_ms = ns_to_ms(c.wait_frame_call_ts, c.wait_frame_return_ts);
            c.begin_frame_ms = ns_to_ms(c.begin_frame_call_ts, c.begin_frame_return_ts);
            c.end_frame_ms = ns_to_ms(c.end_frame_call_ts, c.end_frame_return_ts);
            if (c.begin_frame_return_ts > 0 && c.end_frame_call_ts > 0)
                c.app_work_ms = ns_to_ms(c.begin_frame_return_ts, c.end_frame_call_ts);
            c.total_frame_ms = ns_to_ms(c.wait_frame_call_ts, c.end_frame_return_ts);
            if (c.sync_actions_call_ts > 0 && c.sync_actions_return_ts > 0)
                c.sync_actions_ms = ns_to_ms(c.sync_actions_call_ts, c.sync_actions_return_ts);
            if (c.locate_views_call_ts > 0 && c.locate_views_return_ts > 0)
                c.locate_views_ms = ns_to_ms(c.locate_views_call_ts, c.locate_views_return_ts);

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

XrResult XRAPI_CALL Layer_xrCreateSwapchain(XrSession session,
                                             const XrSwapchainCreateInfo *createInfo,
                                             XrSwapchain *swapchain)
{
    InstanceData *data = GetInstanceDataFromSession(session);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrCreateSwapchain(session, createInfo, swapchain);
    if (XR_SUCCEEDED(result))
        RegisterSwapchain(*swapchain, data);
    return result;
}

XrResult XRAPI_CALL Layer_xrDestroySwapchain(XrSwapchain swapchain)
{
    InstanceData *data = GetInstanceDataFromSwapchain(swapchain);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    XrResult result = data->next.xrDestroySwapchain(swapchain);
    UnregisterSwapchain(swapchain);
    return result;
}

// ── Swapchain image ops ──────────────────────────────────────────────────────

XrResult XRAPI_CALL Layer_xrAcquireSwapchainImage(XrSwapchain swapchain,
                                                    const XrSwapchainImageAcquireInfo *acquireInfo,
                                                    uint32_t *index)
{
    InstanceData *data = GetInstanceDataFromSwapchain(swapchain);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    int64_t t0 = now_ns();
    XrResult result = data->next.xrAcquireSwapchainImage(swapchain, acquireInfo, index);
    int64_t t1 = now_ns();

    if (XR_SUCCEEDED(result)) {
        std::unique_lock lock(data->state_mutex);
        if (data->perf.has_current) {
            SwapchainTiming sc;
            sc.acquire_call_ts = t0;
            sc.acquire_done_ts = t1;
            data->perf.current.swapchains.push_back(sc);
        }
    }
    return result;
}

XrResult XRAPI_CALL Layer_xrWaitSwapchainImage(XrSwapchain swapchain,
                                                 const XrSwapchainImageWaitInfo *waitInfo)
{
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

XrResult XRAPI_CALL Layer_xrReleaseSwapchainImage(XrSwapchain swapchain,
                                                    const XrSwapchainImageReleaseInfo *releaseInfo)
{
    InstanceData *data = GetInstanceDataFromSwapchain(swapchain);
    if (data == nullptr)
        return XR_ERROR_HANDLE_INVALID;

    int64_t t0 = now_ns();
    XrResult result = data->next.xrReleaseSwapchainImage(swapchain, releaseInfo);
    int64_t t1 = now_ns();

    if (XR_SUCCEEDED(result)) {
        std::unique_lock lock(data->state_mutex);
        if (data->perf.has_current && !data->perf.current.swapchains.empty()) {
            auto &sc = data->perf.current.swapchains.back();
            sc.release_ts = t0;
            sc.release_done_ts = t1;
        }
    }
    return result;
}

} // namespace debug_layer
