// SPDX-License-Identifier: MIT
// instance_data.cpp — Global handle map implementation.

#include "instance_data.h"

namespace debug_layer {

// ── Global maps ──────────────────────────────────────────────────────────────

std::mutex g_map_mutex;
std::unordered_map<XrInstance, InstanceData *> g_instance_map;
std::unordered_map<XrSession, InstanceData *> g_session_map;
std::unordered_map<XrActionSet, InstanceData *> g_action_set_map;
std::unordered_map<XrAction, InstanceData *> g_action_map;
std::unordered_map<XrSpace, InstanceData *> g_space_map;
std::unordered_map<XrSwapchain, InstanceData *> g_swapchain_map;

// ── Lookup helpers ───────────────────────────────────────────────────────────

InstanceData *GetInstanceData(XrInstance instance)
{
    std::lock_guard<std::mutex> lock(g_map_mutex);
    auto it = g_instance_map.find(instance);
    return it != g_instance_map.end() ? it->second : nullptr;
}

InstanceData *GetInstanceDataFromSession(XrSession session)
{
    std::lock_guard<std::mutex> lock(g_map_mutex);
    auto it = g_session_map.find(session);
    return it != g_session_map.end() ? it->second : nullptr;
}

InstanceData *GetInstanceDataFromActionSet(XrActionSet actionSet)
{
    std::lock_guard<std::mutex> lock(g_map_mutex);
    auto it = g_action_set_map.find(actionSet);
    return it != g_action_set_map.end() ? it->second : nullptr;
}

InstanceData *GetInstanceDataFromAction(XrAction action)
{
    std::lock_guard<std::mutex> lock(g_map_mutex);
    auto it = g_action_map.find(action);
    return it != g_action_map.end() ? it->second : nullptr;
}

InstanceData *GetInstanceDataFromSpace(XrSpace space)
{
    std::lock_guard<std::mutex> lock(g_map_mutex);
    auto it = g_space_map.find(space);
    return it != g_space_map.end() ? it->second : nullptr;
}

// ── Registration helpers ─────────────────────────────────────────────────────

void RegisterInstance(XrInstance instance, InstanceData *data)
{
    std::lock_guard<std::mutex> lock(g_map_mutex);
    g_instance_map[instance] = data;
}

void UnregisterInstance(XrInstance instance)
{
    std::lock_guard<std::mutex> lock(g_map_mutex);
    g_instance_map.erase(instance);
}

void RegisterSession(XrSession session, InstanceData *data)
{
    std::lock_guard<std::mutex> lock(g_map_mutex);
    g_session_map[session] = data;
}

void UnregisterSession(XrSession session)
{
    std::lock_guard<std::mutex> lock(g_map_mutex);
    g_session_map.erase(session);
}

void RegisterActionSet(XrActionSet actionSet, InstanceData *data)
{
    std::lock_guard<std::mutex> lock(g_map_mutex);
    g_action_set_map[actionSet] = data;
}

void UnregisterActionSet(XrActionSet actionSet)
{
    std::lock_guard<std::mutex> lock(g_map_mutex);
    g_action_set_map.erase(actionSet);
}

void RegisterAction(XrAction action, InstanceData *data)
{
    std::lock_guard<std::mutex> lock(g_map_mutex);
    g_action_map[action] = data;
}

void UnregisterAction(XrAction action)
{
    std::lock_guard<std::mutex> lock(g_map_mutex);
    g_action_map.erase(action);
}

void RegisterSpace(XrSpace space, InstanceData *data)
{
    std::lock_guard<std::mutex> lock(g_map_mutex);
    g_space_map[space] = data;
}

void UnregisterSpace(XrSpace space)
{
    std::lock_guard<std::mutex> lock(g_map_mutex);
    g_space_map.erase(space);
}

InstanceData *GetInstanceDataFromSwapchain(XrSwapchain swapchain)
{
    std::lock_guard<std::mutex> lock(g_map_mutex);
    auto it = g_swapchain_map.find(swapchain);
    return it != g_swapchain_map.end() ? it->second : nullptr;
}

void RegisterSwapchain(XrSwapchain swapchain, InstanceData *data)
{
    std::lock_guard<std::mutex> lock(g_map_mutex);
    g_swapchain_map[swapchain] = data;
}

void UnregisterSwapchain(XrSwapchain swapchain)
{
    std::lock_guard<std::mutex> lock(g_map_mutex);
    g_swapchain_map.erase(swapchain);
}

} // namespace debug_layer
