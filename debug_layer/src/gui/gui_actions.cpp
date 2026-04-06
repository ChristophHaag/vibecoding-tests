// SPDX-License-Identifier: MIT
// gui/gui_actions.cpp — Actions/ActionSets/Bindings/Live State panel.

#include "gui_actions.h"
#include "../instance_data.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <shared_mutex>

namespace debug_layer {

static const char *action_type_str(XrActionType type)
{
    switch (type) {
    case XR_ACTION_TYPE_BOOLEAN_INPUT: return "Boolean";
    case XR_ACTION_TYPE_FLOAT_INPUT: return "Float";
    case XR_ACTION_TYPE_VECTOR2F_INPUT: return "Vector2f";
    case XR_ACTION_TYPE_POSE_INPUT: return "Pose";
    case XR_ACTION_TYPE_VIBRATION_OUTPUT: return "Vibration";
    default: return "Unknown";
    }
}

void gui_render_actions_panel(InstanceData *data)
{
    std::shared_lock lock(data->state_mutex);

    // ── Action Sets ──────────────────────────────────────────────────────
    ImGui::Begin("Action Sets & Actions");

    if (data->action_sets.empty()) {
        ImGui::TextDisabled("No action sets created yet.");
    } else {
        if (ImGui::BeginTable("ActionSets", 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                              ImVec2(0, 200))) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Localized Name");
            ImGui::TableSetupColumn("Priority");
            ImGui::TableSetupColumn("Attached");
            ImGui::TableSetupColumn("Actions");
            ImGui::TableHeadersRow();

            for (auto &[handle, as] : data->action_sets) {
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(as.name.c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(as.localized_name.c_str());

                ImGui::TableNextColumn();
                ImGui::Text("%u", as.priority);

                ImGui::TableNextColumn();
                if (as.attached) {
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Yes");
                } else {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No");
                }

                ImGui::TableNextColumn();
                // Count actions in this set
                int count = 0;
                for (auto &[ah, a] : data->actions) {
                    if (a.parent_action_set == handle)
                        count++;
                }
                ImGui::Text("%d", count);
            }
            ImGui::EndTable();
        }
    }

    ImGui::Separator();

    // ── Actions ──────────────────────────────────────────────────────────
    ImGui::Text("Actions (%zu)", data->actions.size());

    if (!data->actions.empty()) {
        if (ImGui::BeginTable("Actions", 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                              ImVec2(0, 200))) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Localized Name");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Action Set");
            ImGui::TableSetupColumn("Subaction Paths");
            ImGui::TableHeadersRow();

            for (auto &[handle, a] : data->actions) {
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(a.name.c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(a.localized_name.c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(action_type_str(a.type));

                ImGui::TableNextColumn();
                auto asit = data->action_sets.find(a.parent_action_set);
                if (asit != data->action_sets.end())
                    ImGui::TextUnformatted(asit->second.name.c_str());
                else
                    ImGui::TextDisabled("?");

                ImGui::TableNextColumn();
                for (size_t i = 0; i < a.subaction_path_strings.size(); i++) {
                    if (i > 0)
                        ImGui::SameLine();
                    ImGui::TextUnformatted(a.subaction_path_strings[i].c_str());
                }
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();

    // ── Suggested Bindings ───────────────────────────────────────────────
    ImGui::Begin("Suggested Bindings");

    if (data->suggested_bindings.empty()) {
        ImGui::TextDisabled("No bindings suggested yet.");
    } else {
        for (auto &sb : data->suggested_bindings) {
            if (ImGui::TreeNode(sb.interaction_profile.c_str())) {
                if (ImGui::BeginTable("Bindings", 2,
                                      ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                          ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Action");
                    ImGui::TableSetupColumn("Binding Path");
                    ImGui::TableHeadersRow();

                    for (auto &b : sb.bindings) {
                        ImGui::TableNextRow();

                        ImGui::TableNextColumn();
                        auto ait = data->actions.find(b.action);
                        if (ait != data->actions.end())
                            ImGui::TextUnformatted(ait->second.name.c_str());
                        else
                            ImGui::Text("Action %p", (void *)b.action);

                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(b.binding_path.c_str());
                    }
                    ImGui::EndTable();
                }
                ImGui::TreePop();
            }
        }
    }

    ImGui::End();

    // ── Active Interaction Profiles ──────────────────────────────────────
    ImGui::Begin("Active Profiles & Live State");

    if (!data->active_profiles.empty()) {
        ImGui::Text("Active Interaction Profiles:");
        for (auto &ap : data->active_profiles) {
            ImGui::BulletText("%s: %s", ap.subaction_string.c_str(), ap.profile_string.c_str());
        }
        ImGui::Separator();
    }

    // ── Live Action State ────────────────────────────────────────────────
    ImGui::Text("Live Action State (%zu entries)", data->action_states.size());

    if (!data->action_states.empty()) {
        if (ImGui::BeginTable("LiveState", 6,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                              ImVec2(0, 0))) {
            ImGui::TableSetupColumn("Action");
            ImGui::TableSetupColumn("Subaction");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Active");
            ImGui::TableSetupColumn("Value");
            ImGui::TableSetupColumn("Changed");
            ImGui::TableHeadersRow();

            for (auto &[key, state] : data->action_states) {
                ImGui::TableNextRow();

                // Action name
                ImGui::TableNextColumn();
                auto ait = data->actions.find(key.action);
                if (ait != data->actions.end())
                    ImGui::TextUnformatted(ait->second.name.c_str());
                else
                    ImGui::Text("%p", (void *)key.action);

                // Subaction path
                ImGui::TableNextColumn();
                if (key.subaction_path != XR_NULL_PATH)
                    ImGui::TextUnformatted(data->paths.get_string(key.subaction_path).c_str());
                else
                    ImGui::TextDisabled("(all)");

                // Type
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(action_type_str(state.type));

                // Active
                ImGui::TableNextColumn();
                if (state.is_active) {
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "YES");
                } else {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "no");
                }

                // Value
                ImGui::TableNextColumn();
                switch (state.type) {
                case XR_ACTION_TYPE_BOOLEAN_INPUT:
                    if (state.boolean_value)
                        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "TRUE");
                    else
                        ImGui::Text("false");
                    break;

                case XR_ACTION_TYPE_FLOAT_INPUT: {
                    ImGui::Text("%.3f", state.float_value);
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                                          ImVec4(0.3f, 0.7f, 1.0f, 0.8f));
                    ImGui::ProgressBar(state.float_value, ImVec2(80, 0), "");
                    ImGui::PopStyleColor();
                    break;
                }

                case XR_ACTION_TYPE_VECTOR2F_INPUT: {
                    ImGui::Text("(%.2f, %.2f)", state.vector2f_x, state.vector2f_y);
                    // Small crosshair visualization
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    float sz = 30.0f;
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    ImVec2 center(p.x + sz * 0.5f, p.y + sz * 0.5f);
                    // Background
                    dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz),
                                      IM_COL32(40, 40, 60, 200));
                    // Crosshair lines
                    dl->AddLine(ImVec2(center.x, p.y), ImVec2(center.x, p.y + sz),
                                IM_COL32(80, 80, 80, 200));
                    dl->AddLine(ImVec2(p.x, center.y), ImVec2(p.x + sz, center.y),
                                IM_COL32(80, 80, 80, 200));
                    // Dot
                    float dot_x = center.x + state.vector2f_x * sz * 0.5f;
                    float dot_y = center.y - state.vector2f_y * sz * 0.5f;
                    dl->AddCircleFilled(ImVec2(dot_x, dot_y), 3.0f,
                                        IM_COL32(255, 200, 50, 255));
                    ImGui::Dummy(ImVec2(sz, sz));
                    break;
                }

                case XR_ACTION_TYPE_POSE_INPUT:
                    ImGui::TextDisabled("(see Spaces)");
                    break;

                case XR_ACTION_TYPE_VIBRATION_OUTPUT:
                    ImGui::TextDisabled("output");
                    break;

                default: ImGui::TextDisabled("?"); break;
                }

                // Changed
                ImGui::TableNextColumn();
                if (state.changed_since_last_sync)
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "changed");
                else
                    ImGui::TextDisabled("-");
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}

} // namespace debug_layer
